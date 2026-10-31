package feed.pg;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Reads the bytes PgProtocol wants to send, for assertions. */
final class Frontend {

    record Message(char type, byte[] body) {

        String cstring(int at) {
            int end = at;
            while (body[end] != 0) {
                end++;
            }
            return new String(body, at, end - at, StandardCharsets.UTF_8);
        }
    }

    private Frontend() {
    }

    /** Drains everything the protocol has queued to send. */
    static byte[] drain(PgProtocol pg) {
        ByteBuffer out = pg.output();
        byte[] bytes = new byte[out.remaining()];
        out.get(bytes);
        return bytes;
    }

    /** Parses typed messages. */
    static List<Message> messages(byte[] bytes) {
        List<Message> messages = new ArrayList<>();
        ByteBuffer in = ByteBuffer.wrap(bytes);
        while (in.hasRemaining()) {
            char type = (char) in.get();
            byte[] body = new byte[in.getInt() - 4];
            in.get(body);
            messages.add(new Message(type, body));
        }
        return messages;
    }

    /** Parses the StartupMessage (no type byte): protocol version and parameters. */
    static Map<String, String> startup(byte[] bytes) {
        ByteBuffer in = ByteBuffer.wrap(bytes);
        int length = in.getInt();
        if (length != bytes.length) {
            throw new AssertionError("startup length " + length + " but " + bytes.length + " bytes");
        }
        Map<String, String> params = new LinkedHashMap<>();
        params.put("protocol", Integer.toString(in.getInt()));
        while (true) {
            String key = readCString(in);
            if (key.isEmpty()) {
                break;
            }
            params.put(key, readCString(in));
        }
        return params;
    }

    static String types(List<Message> messages) {
        StringBuilder s = new StringBuilder();
        for (Message m : messages) {
            s.append(m.type());
        }
        return s.toString();
    }

    private static String readCString(ByteBuffer in) {
        int start = in.position();
        while (in.get() != 0) {
            // scan
        }
        return new String(in.array(), start, in.position() - start - 1, StandardCharsets.UTF_8);
    }
}
