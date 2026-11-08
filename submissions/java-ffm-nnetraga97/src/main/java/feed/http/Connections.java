package feed.http;

import java.util.Arrays;

import feed.net.FdMap;
import feed.net.Io;

/**
 * A worker's client connections: reading and framing HTTP requests, writing responses, keep-alive and closing.
 *
 * <p>A connection is a slot in the arrays below, not an object, so an idle connection costs a few dozen bytes and
 * owns no buffer: requests are parsed in place from a shared read buffer, and only input that can't be handled yet
 * (a partial request, or one pipelined behind a request in flight) is copied out. Slots are dense and this worker's
 * own; {@link FdMap} finds a socket's slot. Each connection has at most one request in flight, so responses go out
 * in order. A request in flight is identified by a tag holding its slot and the slot's generation, so a result for
 * a client that has since left (its slot reused) is recognized and dropped. A connection whose last response
 * hasn't been fully written handles no new request until it has, so a client that sends without reading only
 * fills its own bounded input buffer.
 */
public final class Connections {

    /** Handles the requests this class frames. */
    public interface RequestHandler {
        /**
         * Handles one complete request, parsed by {@code request} from {@code buffer}. Either answers it at once
         * (through {@link #reply}) and returns false, or returns true and answers later through {@link #complete}.
         */
        boolean handle(int slot, HttpParser request, byte[] buffer);

        /** The connection in {@code slot} closed. */
        void closed(int slot);
    }

    private static final int READ_BUFFER = 64 * 1024;
    private static final int MAX_PENDING_INPUT = 64 * 1024;
    private static final byte[] CONTINUE = ResponseBuffer.ascii("HTTP/1.1 100 Continue\r\n\r\n");
    private static final byte[] BAD_REQUEST = ResponseBuffer.ascii("{\"error\":\"bad request\"}");
    private static final byte[] TOO_LARGE = ResponseBuffer.ascii("{\"error\":\"request too large\"}");

    private final Io io;
    private final Gzip gzip; // null: gzip off
    private final int gzipMin;
    private final HttpParser parser = new HttpParser();
    private final ResponseBuffer fixedBody = new ResponseBuffer(4096);
    private final byte[] readBuffer = new byte[READ_BUFFER];
    private final FdMap slots = new FdMap(1024);
    private RequestHandler handler;
    private int[] freeSlots = new int[64];
    private int freeCount;
    private int nextSlot;

    // Per slot.
    private int[] fd = new int[0];
    private boolean[] open = new boolean[0];
    private int[] generation = new int[0];
    private boolean[] awaiting = new boolean[0];       // a request is in flight
    private boolean[] closeWhenAnswered = new boolean[0];
    private boolean[] closing = new boolean[0];        // close once the pending output is written
    private boolean[] halfClosed = new boolean[0];     // the client stopped sending while a request was in flight
    private boolean[] sentContinue = new boolean[0];
    private byte[] connectionHeader = new byte[0];
    private boolean[] acceptsGzip = new boolean[0];
    private byte[][] input = new byte[0][];
    private int[] inputLength = new int[0];
    private byte[][] output = new byte[0][];
    private int[] outputOffset = new int[0];
    private int[] outputLength = new int[0];

    public Connections(Io io, Gzip gzip, int gzipMin) {
        this.io = io;
        this.gzip = gzip;
        this.gzipMin = gzipMin;
    }

    public void start(RequestHandler handler) {
        this.handler = handler;
    }

    public static int slot(long tag) {
        return (int) tag;
    }

    /** The tag of the request in flight on {@code slot}. */
    public long tag(int slot) {
        return ((long) generation[slot] << 32) | slot;
    }

    /** Whether the request {@code tag} still waits for its answer (its client hasn't left). */
    public boolean isWaiting(long tag) {
        int slot = slot(tag);
        return slot < open.length && open[slot] && generation[slot] == (int) (tag >>> 32) && awaiting[slot];
    }

    // ---- events ----

    public void accepted(int socket) {
        int slot = freeCount > 0 ? freeSlots[--freeCount] : nextSlot++;
        ensureCapacity(slot);
        slots.put(socket, slot);
        fd[slot] = socket;
        open[slot] = true;
        generation[slot]++;
        awaiting[slot] = false;
        closeWhenAnswered[slot] = false;
        closing[slot] = false;
        halfClosed[slot] = false;
        sentContinue[slot] = false;
        inputLength[slot] = 0;
        outputLength[slot] = 0;
    }

    public void readable(int socket) {
        int slot = slots.get(socket);
        if (slot < 0) {
            return;
        }
        try {
            read(slot);
        } catch (RuntimeException e) {
            System.err.println("error on client connection: " + e);
            e.printStackTrace();
            close(slot);
        }
    }

    public void writable(int socket) {
        int slot = slots.get(socket);
        if (slot >= 0) {
            writePending(slot);
        }
    }

    // ---- responses ----

    /** Answers the request being handled with a fixed body (from {@link RequestHandler#handle}). */
    public void reply(int slot, int status, byte[] body) {
        fixedBody.begin();
        fixedBody.raw(body);
        reply(slot, status, fixedBody);
    }

    /** Answers the request being handled with the body in {@code out} (from {@link RequestHandler#handle}). */
    public void reply(int slot, int status, ResponseBuffer out) {
        int start = out.finish(status, connectionHeader[slot]);
        send(slot, out.bytes(), start, out.end() - start);
        answered(slot);
    }

    /** Answers request {@code tag} later, with the body in {@code out}; gzipped when the client accepts it. */
    public void complete(long tag, int status, ResponseBuffer out) {
        if (!isWaiting(tag)) {
            return; // the client left; its slot may already serve someone else
        }
        int slot = slot(tag);
        awaiting[slot] = false;
        boolean compress = gzip != null && acceptsGzip[slot] && out.bodyLength() >= gzipMin;
        int start = out.finish(status, connectionHeader[slot], compress ? gzip : null);
        send(slot, out.bytes(), start, out.end() - start);
        answered(slot);
        if (open[slot] && !closing[slot] && inputLength[slot] > 0) {
            handleBuffered(slot);
        } else {
            closeIfDrained(slot);
        }
    }

    // ---- input ----

    private void read(int slot) {
        int n = io.read(fd[slot], readBuffer, 0, readBuffer.length);
        if (n <= 0) {
            if (n < 0) {
                endOfInput(slot);
            }
            return;
        }
        if (inputLength[slot] == 0) {
            // The common case: handle requests straight from the shared buffer and keep only what's left.
            int used = awaiting[slot] ? 0 : handle(slot, readBuffer, 0, n);
            if (open[slot] && used < n) {
                appendInput(slot, readBuffer, used, n - used);
            }
            return;
        }
        if (inputLength[slot] + n > MAX_PENDING_INPUT) {
            close(slot); // more unanswered input than any client of this API sends
            return;
        }
        appendInput(slot, readBuffer, 0, n);
        if (!awaiting[slot]) {
            handleBuffered(slot);
        }
    }

    /** The client stopped sending: answer what it sent, then close (see {@link #closeIfDrained}). */
    private void endOfInput(int slot) {
        halfClosed[slot] = true;
        io.pauseReads(fd[slot]); // the end of stream stays readable; don't spin on it
        closeIfDrained(slot);
    }

    /** Keeps input that can't be handled yet, in a buffer at most twice what was received. */
    private void appendInput(int slot, byte[] b, int off, int len) {
        int length = inputLength[slot];
        byte[] buffer = input[slot];
        if (buffer == null || buffer.length - length < len) {
            int capacity = Math.min(MAX_PENDING_INPUT, Math.max(length + len, buffer == null ? 0 : buffer.length * 2));
            buffer = buffer == null ? new byte[capacity] : Arrays.copyOf(buffer, capacity);
            input[slot] = buffer;
        }
        System.arraycopy(b, off, buffer, length, len);
        inputLength[slot] = length + len;
    }

    /** Handles the requests waiting in the slot's input buffer, as far as it can. */
    private void handleBuffered(int slot) {
        byte[] buffer = input[slot];
        int length = inputLength[slot];
        int used = handle(slot, buffer, 0, length);
        if (!open[slot]) {
            return;
        }
        if (input[slot] == buffer) {
            System.arraycopy(buffer, used, buffer, 0, length - used);
            inputLength[slot] = length - used;
            if (inputLength[slot] == 0) {
                input[slot] = null; // an idle connection keeps no buffer
            }
        }
        closeIfDrained(slot);
    }

    /** Handles complete requests in {@code b[off, off+len)} until one is in flight; returns the bytes used. */
    private int handle(int slot, byte[] b, int off, int len) {
        int used = 0;
        while (used < len && open[slot] && !awaiting[slot] && !closing[slot] && outputLength[slot] == 0) {
            int n = parser.parse(b, off + used, len - used);
            if (n == HttpParser.INCOMPLETE) {
                if (parser.headersComplete() && parser.expectContinue() && !sentContinue[slot]) {
                    sentContinue[slot] = true;
                    send(slot, CONTINUE, 0, CONTINUE.length);
                }
                break;
            }
            if (n < 0) {
                // A request we can't serve: answer it and close, since the rest of the stream can't be framed.
                connectionHeader[slot] = ResponseBuffer.CLOSE;
                closeWhenAnswered[slot] = true;
                reply(slot, -n, n == -400 ? BAD_REQUEST : TOO_LARGE);
                return len;
            }
            sentContinue[slot] = false;
            closeWhenAnswered[slot] = !parser.keepAlive();
            connectionHeader[slot] = (byte) (!parser.keepAlive() ? ResponseBuffer.CLOSE
                    : parser.http10() ? ResponseBuffer.KEEP_ALIVE : ResponseBuffer.NO_CONNECTION_HEADER);
            acceptsGzip[slot] = parser.acceptsGzip();
            if (handler.handle(slot, parser, b)) {
                awaiting[slot] = true;
            }
            used += n;
        }
        return used;
    }

    // ---- output ----

    private void answered(int slot) {
        if (open[slot] && closeWhenAnswered[slot]) {
            closing[slot] = true;
            if (outputLength[slot] == 0) {
                close(slot);
            }
        }
    }

    /** A half-closed client is closed once everything it sent has been answered and written. */
    private void closeIfDrained(int slot) {
        if (open[slot] && halfClosed[slot] && !awaiting[slot] && outputLength[slot] == 0) {
            close(slot);
        }
    }

    private void send(int slot, byte[] b, int off, int len) {
        if (!open[slot]) {
            return;
        }
        if (outputLength[slot] > 0) {
            queueOutput(slot, b, off, len);
            return;
        }
        int n = io.write(fd[slot], b, off, len);
        if (n < 0) {
            close(slot);
            return;
        }
        if (n < len) {
            queueOutput(slot, b, off + n, len - n);
            io.watchWrites(fd[slot], true);
        }
    }

    private void queueOutput(int slot, byte[] b, int off, int len) {
        byte[] buffer = output[slot];
        int have = outputLength[slot];
        if (have > 0 && outputOffset[slot] > 0) {
            System.arraycopy(buffer, outputOffset[slot], buffer, 0, have);
        }
        outputOffset[slot] = 0;
        if (buffer == null || buffer.length < have + len) {
            buffer = Arrays.copyOf(buffer == null ? new byte[0] : buffer, Math.max(have + len, 8192));
            output[slot] = buffer;
        }
        System.arraycopy(b, off, buffer, have, len);
        outputLength[slot] = have + len;
    }

    private void writePending(int slot) {
        if (outputLength[slot] > 0) {
            int n = io.write(fd[slot], output[slot], outputOffset[slot], outputLength[slot]);
            if (n < 0) {
                close(slot);
                return;
            }
            outputOffset[slot] += n;
            outputLength[slot] -= n;
        }
        if (outputLength[slot] == 0) {
            io.watchWrites(fd[slot], false);
            output[slot] = null;
            if (closing[slot]) {
                close(slot);
            } else if (inputLength[slot] > 0 && !awaiting[slot]) {
                handleBuffered(slot); // requests that waited for the output to drain
            } else {
                closeIfDrained(slot);
            }
        }
    }

    private void close(int slot) {
        if (!open[slot]) {
            return;
        }
        io.close(fd[slot]);
        if (slots.get(fd[slot]) == slot) {
            slots.remove(fd[slot]);
        }
        open[slot] = false;
        generation[slot]++;
        awaiting[slot] = false;
        input[slot] = null;
        inputLength[slot] = 0;
        output[slot] = null;
        outputLength[slot] = 0;
        handler.closed(slot);
        if (freeCount == freeSlots.length) {
            freeSlots = Arrays.copyOf(freeSlots, freeCount * 2);
        }
        freeSlots[freeCount++] = slot;
    }

    private void ensureCapacity(int slot) {
        if (slot < open.length) {
            return;
        }
        int n = Math.max(slot + 1, Math.max(1024, open.length * 2));
        fd = Arrays.copyOf(fd, n);
        open = Arrays.copyOf(open, n);
        generation = Arrays.copyOf(generation, n);
        awaiting = Arrays.copyOf(awaiting, n);
        closeWhenAnswered = Arrays.copyOf(closeWhenAnswered, n);
        closing = Arrays.copyOf(closing, n);
        halfClosed = Arrays.copyOf(halfClosed, n);
        sentContinue = Arrays.copyOf(sentContinue, n);
        connectionHeader = Arrays.copyOf(connectionHeader, n);
        acceptsGzip = Arrays.copyOf(acceptsGzip, n);
        input = Arrays.copyOf(input, n);
        inputLength = Arrays.copyOf(inputLength, n);
        output = Arrays.copyOf(output, n);
        outputOffset = Arrays.copyOf(outputOffset, n);
        outputLength = Arrays.copyOf(outputLength, n);
    }
}
