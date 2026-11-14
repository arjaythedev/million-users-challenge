package challenge;

import java.io.ByteArrayInputStream;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.charset.StandardCharsets;
import java.util.zip.GZIPInputStream;

import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT_UNALIGNED;
import static java.lang.foreign.ValueLayout.JAVA_LONG_UNALIGNED;
import static java.lang.foreign.ValueLayout.JAVA_SHORT_UNALIGNED;

public final class Out {

    private Out() {}

    public static final long BODY_OFFSET      = 128L;
    private static final long GZIP_DEFLATE_OFF = BODY_OFFSET + 10L;

    private static final long  GZIP_HDR_LOW8  = 0x0000000000088B1FL;
    private static final short GZIP_HDR_HIGH2 = (short) 0x0300;
    private static final int   CRLF_CRLF      = 0x0A0D0A0D;

    private static final MemorySegment HDR_200_PLAIN;
    private static final int           HDR_200_PLAIN_LEN;
    private static final MemorySegment HDR_200_GZIP;
    private static final int           HDR_200_GZIP_LEN;

    private static final MemorySegment HDR_201_PLAIN;
    private static final int           HDR_201_PLAIN_LEN;
    private static final MemorySegment HDR_201_GZIP;
    private static final int           HDR_201_GZIP_LEN;

    private static final MemorySegment HDR_503_PLAIN;
    private static final int           HDR_503_PLAIN_LEN;
    private static final MemorySegment HDR_503_GZIP;
    private static final int           HDR_503_GZIP_LEN;

    // Pre-allocated Static Error Responses
    public static final MemorySegment RESP_400_GENERIC;
    public static final int           RESP_400_GENERIC_LEN;

    public static final MemorySegment RESP_400_BAD_ID;
    public static final int           RESP_400_BAD_ID_LEN;

    public static final MemorySegment RESP_400_MALFORMED;
    public static final int           RESP_400_MALFORMED_LEN;

    public static final MemorySegment RESP_400_BODY_REQUIRED;
    public static final int           RESP_400_BODY_REQUIRED_LEN;

    public static final MemorySegment RESP_400_BODY_TOO_LONG;
    public static final int           RESP_400_BODY_TOO_LONG_LEN;

    public static final MemorySegment RESP_401_MISSING;
    public static final int           RESP_401_MISSING_LEN;

    public static final MemorySegment RESP_401_INVALID;
    public static final int           RESP_401_INVALID_LEN;

    public static final MemorySegment RESP_401_PAYLOAD;
    public static final int           RESP_401_PAYLOAD_LEN;

    public static final MemorySegment RESP_404_ROUTE;
    public static final int           RESP_404_ROUTE_LEN;

    public static final MemorySegment RESP_404_POST;
    public static final int           RESP_404_POST_LEN;

    public static final MemorySegment RESP_405;
    public static final int           RESP_405_LEN;

    public static final MemorySegment RESP_500;
    public static final int           RESP_500_LEN;

    public static final MemorySegment RESP_503;
    public static final int           RESP_503_LEN;

    static {
        Arena global = Arena.global();

        byte[] h200p = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                .getBytes(StandardCharsets.US_ASCII);
        HDR_200_PLAIN = allocatePadded(global, h200p);
        HDR_200_PLAIN_LEN = h200p.length;

        byte[] h200g = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Encoding: gzip\r\nContent-Length: "
                .getBytes(StandardCharsets.US_ASCII);
        HDR_200_GZIP = allocatePadded(global, h200g);
        HDR_200_GZIP_LEN = h200g.length;

        byte[] h201p = "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: "
                .getBytes(StandardCharsets.US_ASCII);
        HDR_201_PLAIN = allocatePadded(global, h201p);
        HDR_201_PLAIN_LEN = h201p.length;

        byte[] h201g = "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Encoding: gzip\r\nContent-Length: "
                .getBytes(StandardCharsets.US_ASCII);
        HDR_201_GZIP = allocatePadded(global, h201g);
        HDR_201_GZIP_LEN = h201g.length;

        byte[] h503p = "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Length: "
                .getBytes(StandardCharsets.US_ASCII);
        HDR_503_PLAIN = allocatePadded(global, h503p);
        HDR_503_PLAIN_LEN = h503p.length;

        byte[] h503g = "HTTP/1.1 503 Service Unavailable\r\nContent-Type: application/json\r\nContent-Encoding: gzip\r\nContent-Length: "
                .getBytes(StandardCharsets.US_ASCII);
        HDR_503_GZIP = allocatePadded(global, h503g);
        HDR_503_GZIP_LEN = h503g.length;

        RESP_400_GENERIC = allocateStaticResponse(global, "400 Bad Request", "{\"error\":\"bad request\"}");
        RESP_400_GENERIC_LEN = (int) RESP_400_GENERIC.byteSize();

        RESP_400_BAD_ID = allocateStaticResponse(global, "400 Bad Request", "{\"error\":\"invalid post id\"}");
        RESP_400_BAD_ID_LEN = (int) RESP_400_BAD_ID.byteSize();

        RESP_400_MALFORMED = allocateStaticResponse(global, "400 Bad Request", "{\"error\":\"malformed JSON body\"}");
        RESP_400_MALFORMED_LEN = (int) RESP_400_MALFORMED.byteSize();

        RESP_400_BODY_REQUIRED = allocateStaticResponse(global, "400 Bad Request", "{\"error\":\"body is required\"}");
        RESP_400_BODY_REQUIRED_LEN = (int) RESP_400_BODY_REQUIRED.byteSize();

        RESP_400_BODY_TOO_LONG = allocateStaticResponse(global, "400 Bad Request", "{\"error\":\"body must be at most 500 characters\"}");
        RESP_400_BODY_TOO_LONG_LEN = (int) RESP_400_BODY_TOO_LONG.byteSize();

        RESP_401_MISSING = allocateStaticResponse(global, "401 Unauthorized", "{\"error\":\"missing bearer token\"}");
        RESP_401_MISSING_LEN = (int) RESP_401_MISSING.byteSize();

        RESP_401_INVALID = allocateStaticResponse(global, "401 Unauthorized", "{\"error\":\"invalid or expired token\"}");
        RESP_401_INVALID_LEN = (int) RESP_401_INVALID.byteSize();

        RESP_401_PAYLOAD = allocateStaticResponse(global, "401 Unauthorized", "{\"error\":\"invalid token payload\"}");
        RESP_401_PAYLOAD_LEN = (int) RESP_401_PAYLOAD.byteSize();

        RESP_404_ROUTE = allocateStaticResponse(global, "404 Not Found", "{\"error\":\"not found\"}");
        RESP_404_ROUTE_LEN = (int) RESP_404_ROUTE.byteSize();

        RESP_404_POST = allocateStaticResponse(global, "404 Not Found", "{\"error\":\"post not found\"}");
        RESP_404_POST_LEN = (int) RESP_404_POST.byteSize();

        RESP_405 = allocateStaticResponse(global, "405 Method Not Allowed", "{\"error\":\"method not allowed\"}");
        RESP_405_LEN = (int) RESP_405.byteSize();

        RESP_500 = allocateStaticResponse(global, "500 Internal Server Error", "{\"error\":\"internal server error\"}");
        RESP_500_LEN = (int) RESP_500.byteSize();

        RESP_503 = allocateStaticResponse(global, "503 Service Unavailable", "{\"error\":\"service unavailable\"}");
        RESP_503_LEN = (int) RESP_503.byteSize();
    }

    private static MemorySegment allocatePadded(Arena arena, byte[] bytes) {
        MemorySegment seg = arena.allocate(bytes.length + 8L, 8L);
        MemorySegment.copy(bytes, 0, seg, JAVA_BYTE, 0L, bytes.length);
        return seg;
    }

    private static MemorySegment allocateStaticResponse(Arena arena, String status, String body) {
        byte[] bodyBytes = body.getBytes(StandardCharsets.UTF_8);
        String raw = "HTTP/1.1 " + status + "\r\n"
                   + "Content-Type: application/json\r\n"
                   + "Content-Length: " + bodyBytes.length + "\r\n\r\n"
                   + body;
        byte[] all = raw.getBytes(StandardCharsets.UTF_8);
        MemorySegment seg = arena.allocate(all.length, 8L);
        MemorySegment.copy(all, 0, seg, JAVA_BYTE, 0L, all.length);
        return seg;
    }

    public static long frame200Plain(MemorySegment jsonBuf, int jsonLen) {
        return prependHeader(jsonBuf, HDR_200_PLAIN, HDR_200_PLAIN_LEN, jsonLen);
    }

    public static long frame201Plain(MemorySegment jsonBuf, int jsonLen) {
        return prependHeader(jsonBuf, HDR_201_PLAIN, HDR_201_PLAIN_LEN, jsonLen);
    }

    public static long frame503Plain(MemorySegment jsonBuf, int jsonLen) {
        return prependHeader(jsonBuf, HDR_503_PLAIN, HDR_503_PLAIN_LEN, jsonLen);
    }

    public static long frame200Gzip(
            MemorySegment zstreamSlab, MemorySegment jsonBuf, int jsonLen, MemorySegment outBuf) {
        return compressAndFrameGzip(zstreamSlab, jsonBuf, jsonLen, outBuf, HDR_200_GZIP, HDR_200_GZIP_LEN);
    }

    public static long frame201Gzip(
            MemorySegment zstreamSlab, MemorySegment jsonBuf, int jsonLen, MemorySegment outBuf) {
        return compressAndFrameGzip(zstreamSlab, jsonBuf, jsonLen, outBuf, HDR_201_GZIP, HDR_201_GZIP_LEN);
    }

    public static long frame503Gzip(
            MemorySegment zstreamSlab, MemorySegment jsonBuf, int jsonLen, MemorySegment outBuf) {
        return compressAndFrameGzip(zstreamSlab, jsonBuf, jsonLen, outBuf, HDR_503_GZIP, HDR_503_GZIP_LEN);
    }

    private static long compressAndFrameGzip(
            MemorySegment zstreamSlab,
            MemorySegment jsonBuf, int jsonLen,
            MemorySegment outBuf,
            MemorySegment hdrPrefix, int hdrPrefixLen) {

        outBuf.set(JAVA_LONG_UNALIGNED, BODY_OFFSET, GZIP_HDR_LOW8);
        outBuf.set(JAVA_SHORT_UNALIGNED, BODY_OFFSET + 8L, GZIP_HDR_HIGH2);

        int maxOut = (int) (outBuf.byteSize() - GZIP_DEFLATE_OFF);
        int deflatePlusTrailer = Sys.isalDeflateGzipNoHdr(
            zstreamSlab,
            jsonBuf, BODY_OFFSET, jsonLen,
            outBuf, GZIP_DEFLATE_OFF, maxOut
        );
        int totalGzipLen = 10 + deflatePlusTrailer;

        return prependHeader(outBuf, hdrPrefix, hdrPrefixLen, totalGzipLen);
    }

    private static long prependHeader(
            MemorySegment buf,
            MemorySegment hdrPrefix, int hdrPrefixLen,
            int contentLen) {

        int digits = asciiDigits(contentLen);
        int totalHdrLen = hdrPrefixLen + digits + 4;
        long startOff = BODY_OFFSET - totalHdrLen;

        int i = 0;
        for (; i + 8 <= hdrPrefixLen; i += 8) {
            buf.set(JAVA_LONG_UNALIGNED, startOff + i, hdrPrefix.get(JAVA_LONG_UNALIGNED, i));
        }
        for (; i < hdrPrefixLen; i++) {
            buf.set(JAVA_BYTE, startOff + i, hdrPrefix.get(JAVA_BYTE, i));
        }

        Json.writeIntAscii(buf, startOff + hdrPrefixLen, contentLen);
        buf.set(JAVA_INT_UNALIGNED, BODY_OFFSET - 4L, CRLF_CRLF);

        int totalPacketLen = totalHdrLen + contentLen;
        return (startOff << 32) | (totalPacketLen & 0xFFFFFFFFL);
    }

    public static long packetOffset(long packedHandle) {
        return packedHandle >>> 32;
    }

    public static int packetLength(long packedHandle) {
        return (int) packedHandle;
    }

    private static int asciiDigits(int v) {
        if (v < 10) return 1;
        if (v < 100) return 2;
        if (v < 1000) return 3;
        if (v < 10000) return 4;
        if (v < 100000) return 5;
        if (v < 1000000) return 6;
        return 7;
    }
}
