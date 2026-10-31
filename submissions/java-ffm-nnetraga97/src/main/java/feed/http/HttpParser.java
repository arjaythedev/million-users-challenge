package feed.http;

/**
 * Parses one HTTP/1.x request from a byte range, in place: the fields are offsets into the caller's buffer. Handles
 * Content-Length bodies, keep-alive (1.1 default, 1.0 opt-in), {@code Connection: close}, {@code Expect:
 * 100-continue}, bare LF line ends and pipelined requests (it reports only the first). Chunked request bodies are
 * refused with 400: nothing in this API sends them. Reusable, one per worker.
 */
public final class HttpParser {

    public static final int INCOMPLETE = 0;

    public static final int GET = 1;
    public static final int POST = 2;
    public static final int OTHER = 3;

    public static final int MAX_HEAD = 16 * 1024;
    public static final int MAX_BODY = 16 * 1024;

    private int method;
    private int pathOff;
    private int pathLen;
    private boolean http10;
    private boolean keepAlive;
    private boolean headersComplete;
    private boolean expectContinue;
    private boolean acceptGzip;
    private int authOff;
    private int authLen;
    private int bodyOff;
    private int bodyLen;

    // Accept-Encoding, over every such header: 1 = listed with q > 0, 0 = listed with q = 0, -1 = not listed.
    private int gzipQ;
    private int anyQ;

    /**
     * Parses {@code b[off, off+len)}: returns the request's total length when it is complete, INCOMPLETE (0) when
     * more bytes are needed, or a negative HTTP status for a request that can't be served (-400, -413, -431).
     */
    public int parse(byte[] b, int off, int len) {
        int end = off + len;
        headersComplete = false;
        expectContinue = false;
        acceptGzip = false;
        gzipQ = -1;
        anyQ = -1;
        authOff = 0;
        authLen = -1;
        bodyLen = 0;

        int headEnd = headEnd(b, off, Math.min(end, off + MAX_HEAD));
        if (headEnd < 0) {
            return len >= MAX_HEAD ? -431 : INCOMPLETE;
        }

        // Request line: METHOD SP target SP HTTP/1.x
        int lineEnd = lineEnd(b, off, headEnd);
        int sp1 = Bytes.indexOf(b, off, lineEnd, (byte) ' ');
        int sp2 = sp1 < 0 ? -1 : Bytes.indexOf(b, sp1 + 1, lineEnd, (byte) ' ');
        if (sp2 < 0 || sp2 - sp1 < 2 || b[sp1 + 1] != '/') {
            return -400;
        }
        method = Bytes.equals(b, off, sp1, "GET") ? GET : Bytes.equals(b, off, sp1, "POST") ? POST : OTHER;
        pathOff = sp1 + 1;
        int query = Bytes.indexOf(b, pathOff, sp2, (byte) '?');
        pathLen = (query < 0 ? sp2 : query) - pathOff;
        int versionEnd = trimEnd(b, sp2 + 1, lineEnd);
        if (Bytes.equals(b, sp2 + 1, versionEnd, "HTTP/1.1")) {
            http10 = false;
            keepAlive = true;
        } else if (Bytes.equals(b, sp2 + 1, versionEnd, "HTTP/1.0")) {
            http10 = true;
            keepAlive = false;
        } else {
            return -400;
        }

        // Header fields.
        long contentLength = -1;
        int line = nextLine(b, lineEnd, headEnd);
        while (line < headEnd) {
            int eol = lineEnd(b, line, headEnd);
            int valueEnd = trimEnd(b, line, eol);
            if (valueEnd > line) {
                int colon = Bytes.indexOf(b, line, valueEnd, (byte) ':');
                if (colon <= line || !isToken(b, line, colon)) {
                    return -400; // no name, or a name with whitespace/control bytes (RFC 9110 field-name = token)
                }
                int value = colon + 1;
                while (value < valueEnd && (b[value] == ' ' || b[value] == '\t')) {
                    value++;
                }
                switch (colon - line) {
                    case 6 -> {
                        if (nameIs(b, line, colon, "expect") && containsIgnoreCase(b, value, valueEnd, "100-continue")) {
                            expectContinue = true;
                        }
                    }
                    case 10 -> {
                        if (nameIs(b, line, colon, "connection")) {
                            if (containsIgnoreCase(b, value, valueEnd, "close")) {
                                keepAlive = false;
                            } else if (containsIgnoreCase(b, value, valueEnd, "keep-alive")) {
                                keepAlive = true;
                            }
                        }
                    }
                    case 13 -> {
                        if (nameIs(b, line, colon, "authorization")) {
                            authOff = value;
                            authLen = valueEnd - value;
                        }
                    }
                    case 14 -> {
                        if (nameIs(b, line, colon, "content-length")) {
                            long n = digits(b, value, valueEnd);
                            if (n < 0 || (contentLength >= 0 && n != contentLength)) {
                                return -400; // malformed, or two Content-Lengths that disagree
                            }
                            contentLength = n;
                        }
                    }
                    case 15 -> {
                        if (nameIs(b, line, colon, "accept-encoding")) {
                            acceptEncoding(b, value, valueEnd);
                        }
                    }
                    case 17 -> {
                        if (nameIs(b, line, colon, "transfer-encoding")) {
                            return -400;
                        }
                    }
                    default -> {
                    }
                }
            }
            line = nextLine(b, eol, headEnd);
        }
        if (contentLength < 0) {
            contentLength = 0;
        }
        if (contentLength > MAX_BODY) {
            return -413;
        }

        headersComplete = true;
        acceptGzip = gzipQ > 0 || (gzipQ < 0 && anyQ > 0); // an explicit "gzip;q=0" beats "*"
        bodyOff = headEnd;
        bodyLen = (int) contentLength;
        long total = (long) headEnd - off + contentLength;
        return total <= len ? (int) total : INCOMPLETE;
    }

    /** One Accept-Encoding value (RFC 9110 12.5.3): records gzip (or x-gzip) and * with whether their q is above 0. */
    private void acceptEncoding(byte[] b, int from, int to) {
        while (from < to) {
            int comma = Bytes.indexOf(b, from, to, (byte) ',');
            int end = comma < 0 ? to : comma;
            int semi = Bytes.indexOf(b, from, end, (byte) ';');
            int nameStart = skipSpace(b, from, end);
            int nameEnd = trimEnd(b, nameStart, semi < 0 ? end : semi);
            int q = semi < 0 || qAboveZero(b, semi + 1, end) ? 1 : 0;
            if (nameIs(b, nameStart, nameEnd, "gzip") || nameIs(b, nameStart, nameEnd, "x-gzip")) {
                gzipQ = Math.max(gzipQ, q);
            } else if (nameIs(b, nameStart, nameEnd, "*")) {
                anyQ = Math.max(anyQ, q);
            }
            from = end + 1;
        }
    }

    /** False only for a "q" parameter whose value is zero ("0", "0.0", "0.000"). */
    private static boolean qAboveZero(byte[] b, int from, int to) {
        while (from < to) {
            int semi = Bytes.indexOf(b, from, to, (byte) ';');
            int end = semi < 0 ? to : semi;
            int eq = Bytes.indexOf(b, from, end, (byte) '=');
            if (eq > 0) {
                int nameStart = skipSpace(b, from, eq);
                if (nameIs(b, nameStart, trimEnd(b, nameStart, eq), "q")) {
                    int v = skipSpace(b, eq + 1, end);
                    int vEnd = trimEnd(b, v, end);
                    boolean zero = v < vEnd;
                    for (int i = v; i < vEnd; i++) {
                        zero &= b[i] == '0' || b[i] == '.';
                    }
                    return !zero;
                }
            }
            from = end + 1;
        }
        return true;
    }

    private static int skipSpace(byte[] b, int from, int to) {
        while (from < to && (b[from] == ' ' || b[from] == '\t')) {
            from++;
        }
        return from;
    }

    /** {@link #GET}, {@link #POST} or {@link #OTHER}. */
    public int method() {
        return method;
    }

    /** Where the path starts (the query string is left out). */
    public int pathStart() {
        return pathOff;
    }

    public int pathEnd() {
        return pathOff + pathLen;
    }

    public boolean http10() {
        return http10;
    }

    public boolean keepAlive() {
        return keepAlive;
    }

    /** The head is complete though the body isn't (set when {@link #parse} returns {@link #INCOMPLETE}). */
    public boolean headersComplete() {
        return headersComplete;
    }

    public boolean expectContinue() {
        return expectContinue;
    }

    public boolean acceptsGzip() {
        return acceptGzip;
    }

    /** The Authorization header's value starts here; {@link #authorizationLength()} is -1 without one. */
    public int authorizationStart() {
        return authOff;
    }

    public int authorizationLength() {
        return authLen;
    }

    public int bodyStart() {
        return bodyOff;
    }

    public int bodyEnd() {
        return bodyOff + bodyLen;
    }

    /** The offset just past the blank line ending the head, or -1. */
    private static int headEnd(byte[] b, int from, int to) {
        for (int i = from; i < to; i++) {
            if (b[i] == '\n') {
                if (i + 1 < to && b[i + 1] == '\n') {
                    return i + 2;
                }
                if (i + 2 < to && b[i + 1] == '\r' && b[i + 2] == '\n') {
                    return i + 3;
                }
            }
        }
        return -1;
    }

    /** End of the line starting at {@code from}, before its CR/LF. */
    private static int lineEnd(byte[] b, int from, int to) {
        int lf = Bytes.indexOf(b, from, to, (byte) '\n');
        int end = lf < 0 ? to : lf;
        return end > from && b[end - 1] == '\r' ? end - 1 : end;
    }

    /** Start of the line after the one ending at {@code eol}. */
    private static int nextLine(byte[] b, int eol, int to) {
        int lf = Bytes.indexOf(b, eol, to, (byte) '\n');
        return lf < 0 ? to : lf + 1;
    }

    private static int trimEnd(byte[] b, int from, int to) {
        while (to > from && (b[to - 1] == ' ' || b[to - 1] == '\t' || b[to - 1] == '\r')) {
            to--;
        }
        return to;
    }

    /** Case-insensitive compare with a lower-case ASCII string: only A-Z fold, every other byte must match exactly. */
    private static boolean nameIs(byte[] b, int from, int to, String lower) {
        if (to - from != lower.length()) {
            return false;
        }
        for (int i = 0; i < lower.length(); i++) {
            int c = b[from + i];
            if (c >= 'A' && c <= 'Z') {
                c |= 0x20;
            }
            if (c != lower.charAt(i)) {
                return false;
            }
        }
        return true;
    }

    /** RFC 9110 token: letters, digits and !#$%&'*+-.^_`|~ only. */
    private static boolean isToken(byte[] b, int from, int to) {
        for (int i = from; i < to; i++) {
            int c = b[i];
            boolean ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
                    || (c > 0 && "!#$%&'*+-.^_`|~".indexOf(c) >= 0);
            if (!ok) {
                return false;
            }
        }
        return true;
    }

    private static boolean containsIgnoreCase(byte[] b, int from, int to, String lower) {
        int n = lower.length();
        for (int i = from; i + n <= to; i++) {
            if (nameIs(b, i, i + n, lower)) {
                return true;
            }
        }
        return false;
    }

    /** A non-negative decimal of at most 9 digits, or -1. */
    private static long digits(byte[] b, int from, int to) {
        if (from == to || to - from > 9) {
            return to - from > 9 && allDigits(b, from, to) ? Long.MAX_VALUE : -1;
        }
        long v = 0;
        for (int i = from; i < to; i++) {
            if (b[i] < '0' || b[i] > '9') {
                return -1;
            }
            v = v * 10 + (b[i] - '0');
        }
        return v;
    }

    private static boolean allDigits(byte[] b, int from, int to) {
        for (int i = from; i < to; i++) {
            if (b[i] < '0' || b[i] > '9') {
                return false;
            }
        }
        return true;
    }
}
