package challenge;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import jdk.incubator.vector.ByteVector;
import jdk.incubator.vector.VectorOperators;
import jdk.incubator.vector.VectorSpecies;

import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT_UNALIGNED;
import static java.lang.foreign.ValueLayout.JAVA_LONG_UNALIGNED;
import static java.lang.foreign.ValueLayout.JAVA_SHORT_UNALIGNED;

public final class Json {

    private Json() {}

    private static final VectorSpecies<Byte> SPECIES_256 = ByteVector.SPECIES_256;

    // =========================================================================
    // Little-Endian Unaligned Word Constants for SPEC.md JSON Templates
    // =========================================================================
    // `{"posts":[` (10 bytes): long `{"posts"` (0x227374736F70227BL) + short `:[` (0x5B3A)
    private static final long  TPL_FEED_START_8 = 0x227374736F70227BL;
    private static final short TPL_FEED_START_2 = (short) 0x5B3A;
    // `]}` (2 bytes)
    private static final short TPL_FEED_END_2   = (short) 0x7D5D;

    // `{"post":` (8 bytes): 0x3A2274736F70227BL
    private static final long  TPL_SINGLE_START_8 = 0x3A2274736F70227BL;

    // `{"id":` (6 bytes): int `{"id` (0x6469227B) + short `":` (0x3A22)
    private static final int   TPL_OPEN_ID_4    = 0x6469227B;
    private static final short TPL_OPEN_ID_2    = (short) 0x3A22;

    // `,"body":"` (9 bytes): long `,"body":` (0x3A2279646F62222CL) + byte `"` (0x22)
    private static final long  TPL_BODY_8       = 0x3A2279646F62222CL;

    // `","created_at":"` (16 bytes)
    private static final long  TPL_CREATED_8A   = 0x7461657263222C22L;
    private static final long  TPL_CREATED_8B   = 0x223A2274615F6465L;

    // `","author":"` (12 bytes): long `","autho` (0x6F68747561222C22L) + int `r":"` (0x223A2272)
    private static final long  TPL_AUTHOR_8     = 0x6F68747561222C22L;
    private static final int   TPL_AUTHOR_4     = 0x223A2272;

    // `","like_count":` (15 bytes): long `","like_` at +0, overlapping long `_count":` at +7
    private static final long  TPL_LIKES_8A     = 0x5F656B696C222C22L;
    private static final long  TPL_LIKES_8B     = 0x3A22746E756F635FL;

    // `","like_count":0}}` (18 bytes) for POST /posts 201 Created response
    private static final int   TPL_ZERO_CLOSE_4 = 0x7D7D303A; // `:0}}` at +14

    private static final byte[] LOC_HEALTH_OK_PREFIX =
            "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] LOC_HEALTH_ERR_BODY =
            "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"database unreachable\"}".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] LOC_LIKE_NEW_PREFIX =
            "{\"liked\":true,\"already_liked\":false,\"post_id\":".getBytes(StandardCharsets.US_ASCII);
    private static final byte[] LOC_LIKE_OLD_PREFIX =
            "{\"liked\":true,\"already_liked\":true,\"post_id\":".getBytes(StandardCharsets.US_ASCII);

    private static final byte[] HEX_DIGITS  = "0123456789abcdef".getBytes(StandardCharsets.US_ASCII);
    private static final short[] DIGIT_PAIRS = new short[100];

    static {
        for (int i = 0; i < 100; i++) {
            int tens = (i / 10) + '0';
            int ones = (i % 10) + '0';
            DIGIT_PAIRS[i] = (short) (tens | (ones << 8));
        }
        selfTest();
    }

    // =========================================================================
    // Envelope Framing Helpers forPg.java
    // =========================================================================

    public static long startFeedObject(MemorySegment dst, long pos) {
        dst.set(JAVA_LONG_UNALIGNED, pos, TPL_FEED_START_8);
        dst.set(JAVA_SHORT_UNALIGNED, pos + 8L, TPL_FEED_START_2);
        return pos + 10L;
    }

    public static long endFeedObject(MemorySegment dst, long pos) {
        dst.set(JAVA_SHORT_UNALIGNED, pos, TPL_FEED_END_2);
        return pos + 2L;
    }

    public static long startSinglePostObject(MemorySegment dst, long pos) {
        dst.set(JAVA_LONG_UNALIGNED, pos, TPL_SINGLE_START_8);
        return pos + 8L;
    }

    public static long endSinglePostObject(MemorySegment dst, long pos) {
        dst.set(JAVA_BYTE, pos, (byte) '}');
        return pos + 1L;
    }

    /**
     * Serializes a single post object directly from Postgres DataRow text column slices:
     * {"id":<id>,"body":"<body>","created_at":"<iso_ms>","author":"<author>","like_count":<likes>}
     */
    public static long writePostFromPgText(
            MemorySegment dst, long pos, boolean prependComma,
            MemorySegment pgBuf,
            long idOff, int idLen,
            long bodyOff, int bodyLen,
            long createdOff, int createdLen,
            long authorOff, int authorLen,
            long likesOff, int likesLen) {

        if (prependComma) {
            dst.set(JAVA_BYTE, pos++, (byte) ',');
        }

        // 1. `{"id":` (6B) + id
        dst.set(JAVA_INT_UNALIGNED, pos, TPL_OPEN_ID_4);
        dst.set(JAVA_SHORT_UNALIGNED, pos + 4L, TPL_OPEN_ID_2);
        pos += 6L;
        pos = writeNumberSlice(pgBuf, idOff, idLen, dst, pos);

        // 2. `,"body":"` (9B) + escaped body
        dst.set(JAVA_LONG_UNALIGNED, pos, TPL_BODY_8);
        dst.set(JAVA_BYTE, pos + 8L, (byte) '"');
        pos += 9L;
        pos = writeJsonEscapedString(pgBuf, bodyOff, bodyLen, dst, pos);

        // 3. `","created_at":"` (16B) + 24-byte ISO-8601 timestamp (.mmmZ)
        dst.set(JAVA_LONG_UNALIGNED, pos, TPL_CREATED_8A);
        dst.set(JAVA_LONG_UNALIGNED, pos + 8L, TPL_CREATED_8B);
        pos += 16L;
        pos = writePgTimestampIso(pgBuf, createdOff, createdLen, dst, pos);

        // 4. `","author":"` (12B) + escaped author
        dst.set(JAVA_LONG_UNALIGNED, pos, TPL_AUTHOR_8);
        dst.set(JAVA_INT_UNALIGNED, pos + 8L, TPL_AUTHOR_4);
        pos += 12L;
        pos = writeJsonEscapedString(pgBuf, authorOff, authorLen, dst, pos);

        // 5. `","like_count":` (15B) + like_count + `}`
        dst.set(JAVA_LONG_UNALIGNED, pos, TPL_LIKES_8A);
        dst.set(JAVA_LONG_UNALIGNED, pos + 7L, TPL_LIKES_8B);
        pos += 15L;
        pos = writeNumberSlice(pgBuf, likesOff, likesLen, dst, pos);

        dst.set(JAVA_BYTE, pos, (byte) '}');
        return pos + 1L;
    }

    /**
     * Serializes the full 201 Created response for POST /posts:
     * {"post":{"id":<id>,"body":"<body>","created_at":"<iso>","author":"<author>","like_count":0}}
     */
    public static long writeCreatedPostObject(
            MemorySegment dst, long pos,
            MemorySegment pgRxBuf, long idOff, int idLen,
            long createdOff, int createdLen,
            MemorySegment reqCopyBuf, long bodyOff, int bodyLen,
            long authorOff, int authorLen) {

        // `{"post":` (8B) + `{"id":` (6B)
        dst.set(JAVA_LONG_UNALIGNED, pos, TPL_SINGLE_START_8);
        dst.set(JAVA_INT_UNALIGNED, pos + 8L, TPL_OPEN_ID_4);
        dst.set(JAVA_SHORT_UNALIGNED, pos + 12L, TPL_OPEN_ID_2);
        pos += 14L;
        pos = writeNumberSlice(pgRxBuf, idOff, idLen, dst, pos);

        // `,"body":"` (9B) + body
        dst.set(JAVA_LONG_UNALIGNED, pos, TPL_BODY_8);
        dst.set(JAVA_BYTE, pos + 8L, (byte) '"');
        pos += 9L;
        pos = writeJsonEscapedString(reqCopyBuf, bodyOff, bodyLen, dst, pos);

        // `","created_at":"` (16B) + timestamp
        dst.set(JAVA_LONG_UNALIGNED, pos, TPL_CREATED_8A);
        dst.set(JAVA_LONG_UNALIGNED, pos + 8L, TPL_CREATED_8B);
        pos += 16L;
        pos = writePgTimestampIso(pgRxBuf, createdOff, createdLen, dst, pos);

        // `","author":"` (12B) + author
        dst.set(JAVA_LONG_UNALIGNED, pos, TPL_AUTHOR_8);
        dst.set(JAVA_INT_UNALIGNED, pos + 8L, TPL_AUTHOR_4);
        pos += 12L;
        pos = writeJsonEscapedString(reqCopyBuf, authorOff, authorLen, dst, pos);

        // `","like_count":0}}` (18B)
        dst.set(JAVA_LONG_UNALIGNED, pos, TPL_LIKES_8A);
        dst.set(JAVA_LONG_UNALIGNED, pos + 7L, TPL_LIKES_8B);
        dst.set(JAVA_INT_UNALIGNED, pos + 14L, TPL_ZERO_CLOSE_4);
        return pos + 18L;
    }

    /**
     * Serializes the POST /posts/:id/like response:
     * {"liked":true,"already_liked":false,"post_id":123}
     */
    public static long writeLikeResponse(MemorySegment dst, long pos, boolean alreadyLiked, long postId) {
        byte[] prefix = alreadyLiked ? LOC_LIKE_OLD_PREFIX : LOC_LIKE_NEW_PREFIX;
        MemorySegment.copy(prefix, 0, dst, JAVA_BYTE, pos, prefix.length);
        pos += prefix.length;
        pos = writeLongAscii(dst, pos, postId);
        dst.set(JAVA_BYTE, pos, (byte) '}');
        return pos + 1L;
    }

    /**
     * Serializes the GET /health response:
     * 200: {"status":"ok","db":"ok","uptime_s":123}
     * 503: {"status":"degraded","db":"unreachable","error":"database unreachable"}
     */
    public static long writeHealthResponse(MemorySegment dst, long pos, boolean dbOk, long uptimeSec) {
        if (dbOk) {
            MemorySegment.copy(LOC_HEALTH_OK_PREFIX, 0, dst, JAVA_BYTE, pos, LOC_HEALTH_OK_PREFIX.length);
            pos += LOC_HEALTH_OK_PREFIX.length;
            pos = writeLongAscii(dst, pos, Math.max(0L, uptimeSec));
            dst.set(JAVA_BYTE, pos, (byte) '}');
            return pos + 1L;
        }
        MemorySegment.copy(LOC_HEALTH_ERR_BODY, 0, dst, JAVA_BYTE, pos, LOC_HEALTH_ERR_BODY.length);
        return pos + LOC_HEALTH_ERR_BODY.length;
    }

    private static long writeNumberSlice(
            MemorySegment src, long srcOff, int len, MemorySegment dst, long dstPos) {
        if (len <= 8 && srcOff + 8L <= src.byteSize()) {
            dst.set(JAVA_LONG_UNALIGNED, dstPos, src.get(JAVA_LONG_UNALIGNED, srcOff));
            return dstPos + len;
        }
        for (int i = 0; i < len; i++) {
            dst.set(JAVA_BYTE, dstPos + i, src.get(JAVA_BYTE, srcOff + i));
        }
        return dstPos + len;
    }

    /**
     * Normalizes any Postgres text timestamptz ("YYYY-MM-DD HH:MM:SS+00",
     * "YYYY-MM-DD HH:MM:SS.4+00", "YYYY-MM-DD HH:MM:SS.409+00", or "YYYY-MM-DD HH:MM:SS.123456+00")
     * into exact 24-byte ISO-8601 millisecond format ("YYYY-MM-DDTHH:MM:SS.sssZ") in 3 word stores.
     */
    public static long writePgTimestampIso(
            MemorySegment src, long tsOff, int tsLen, MemorySegment dst, long dstPos) {

        if (tsLen < 19) {
            for (int i = 0; i < tsLen; i++) {
                dst.set(JAVA_BYTE, dstPos + i, src.get(JAVA_BYTE, tsOff + i));
            }
            return dstPos + tsLen;
        }

        // Copy first 19 bytes ("YYYY-MM-DD HH:MM:SS") and set byte 10 to 'T'
        long w0 = src.get(JAVA_LONG_UNALIGNED, tsOff);
        long w1 = src.get(JAVA_LONG_UNALIGNED, tsOff + 8L);
        int tailTime = src.get(JAVA_INT_UNALIGNED, tsOff + 16L) & 0x00FFFFFF; // ":SS" (3 bytes)

        dst.set(JAVA_LONG_UNALIGNED, dstPos, w0);
        dst.set(JAVA_LONG_UNALIGNED, dstPos + 8L, w1);
        dst.set(JAVA_BYTE, dstPos + 10L, (byte) 'T');

        // Parse 0..3 millisecond digits after byte 19 (stopping before '+'/'-'/'Z')
        int d0 = '0', d1 = '0', d2 = '0';
        if (tsLen > 19 && src.get(JAVA_BYTE, tsOff + 19L) == '.') {
            if (tsLen > 20) {
                int c0 = src.get(JAVA_BYTE, tsOff + 20L) & 0xFF;
                if (c0 >= '0' && c0 <= '9') {
                    d0 = c0;
                    if (tsLen > 21) {
                        int c1 = src.get(JAVA_BYTE, tsOff + 21L) & 0xFF;
                        if (c1 >= '0' && c1 <= '9') {
                            d1 = c1;
                            if (tsLen > 22) {
                                int c2 = src.get(JAVA_BYTE, tsOff + 22L) & 0xFF;
                                if (c2 >= '0' && c2 <= '9') {
                                    d2 = c2;
                                }
                            }
                        }
                    }
                }
            }
        }

        // Pack ":SS.d0d1d2Z" into a single 64-bit little-endian word at dstPos + 16
        long w2 = Integer.toUnsignedLong(tailTime)
                | (((long) '.') << 24)
                | (((long) d0)  << 32)
                | (((long) d1)  << 40)
                | (((long) d2)  << 48)
                | (((long) 'Z') << 56);
        dst.set(JAVA_LONG_UNALIGNED, dstPos + 16L, w2);
        return dstPos + 24L;
    }

    // =========================================================================
    // Hybrid Vector API + SWAR JSON String Escaper
    // =========================================================================

    public static long writeJsonEscapedString(
            MemorySegment src, long srcOff, int len, MemorySegment dst, long dstPos) {

        int i = 0;
        if (len >= 32) {
            int vecLimit = len - 32;
            for (; i <= vecLimit; i += 32) {
                ByteVector v = ByteVector.fromMemorySegment(SPECIES_256, src, srcOff + i, ByteOrder.nativeOrder());
                var mask = v.compare(VectorOperators.ULT, (byte) 0x20)
                            .or(v.eq((byte) '"'))
                            .or(v.eq((byte) '\\'));
                if (mask.anyTrue()) {
                    int first = mask.firstTrue();
                    for (int k = 0; k < first; k++) {
                        dst.set(JAVA_BYTE, dstPos++, src.get(JAVA_BYTE, srcOff + i + k));
                    }
                    return writeEscapedSlow(src, srcOff, len, i + first, dst, dstPos);
                }
                v.intoMemorySegment(dst, dstPos, ByteOrder.nativeOrder());
                dstPos += 32L;
            }
        }

        for (; i <= len - 8; i += 8) {
            long word = src.get(JAVA_LONG_UNALIGNED, srcOff + i);
            if (hasEscapeByteSwar(word)) {
                return writeEscapedSlow(src, srcOff, len, i, dst, dstPos);
            }
            dst.set(JAVA_LONG_UNALIGNED, dstPos, word);
            dstPos += 8L;
        }

        for (; i < len; i++) {
            byte b = src.get(JAVA_BYTE, srcOff + i);
            int u = b & 0xFF;
            if (u < 0x20 || u == '"' || u == '\\') {
                return writeEscapedSlow(src, srcOff, len, i, dst, dstPos);
            }
            dst.set(JAVA_BYTE, dstPos++, b);
        }

        return dstPos;
    }

    private static boolean hasEscapeByteSwar(long w) {
        long asciiMask = ~w & 0x8080808080808080L;
        long lt20 = ((w & 0x7F7F7F7F7F7F7F7FL) + 0x6060606060606060L) & asciiMask;

        long xorQuote = w ^ 0x2222222222222222L;
        long eqQuote = (xorQuote - 0x0101010101010101L) & ~xorQuote & 0x8080808080808080L;

        long xorSlash = w ^ 0x5C5C5C5C5C5C5C5CL;
        long eqSlash = (xorSlash - 0x0101010101010101L) & ~xorSlash & 0x8080808080808080L;

        return ((lt20 ^ asciiMask) | eqQuote | eqSlash) != 0L;
    }

    private static long writeEscapedSlow(
            MemorySegment src, long srcOff, int len, int startIdx, MemorySegment dst, long dstPos) {
        for (int i = startIdx; i < len; i++) {
            int u = src.get(JAVA_BYTE, srcOff + i) & 0xFF;
            switch (u) {
                case '"' -> {
                    dst.set(JAVA_BYTE, dstPos++, (byte) '\\');
                    dst.set(JAVA_BYTE, dstPos++, (byte) '"');
                }
                case '\\' -> {
                    dst.set(JAVA_BYTE, dstPos++, (byte) '\\');
                    dst.set(JAVA_BYTE, dstPos++, (byte) '\\');
                }
                case '\n' -> {
                    dst.set(JAVA_BYTE, dstPos++, (byte) '\\');
                    dst.set(JAVA_BYTE, dstPos++, (byte) 'n');
                }
                case '\r' -> {
                    dst.set(JAVA_BYTE, dstPos++, (byte) '\\');
                    dst.set(JAVA_BYTE, dstPos++, (byte) 'r');
                }
                case '\t' -> {
                    dst.set(JAVA_BYTE, dstPos++, (byte) '\\');
                    dst.set(JAVA_BYTE, dstPos++, (byte) 't');
                }
                case '\b' -> {
                    dst.set(JAVA_BYTE, dstPos++, (byte) '\\');
                    dst.set(JAVA_BYTE, dstPos++, (byte) 'b');
                }
                case '\f' -> {
                    dst.set(JAVA_BYTE, dstPos++, (byte) '\\');
                    dst.set(JAVA_BYTE, dstPos++, (byte) 'f');
                }
                default -> {
                    if (u < 0x20) {
                        dst.set(JAVA_BYTE, dstPos++, (byte) '\\');
                        dst.set(JAVA_BYTE, dstPos++, (byte) 'u');
                        dst.set(JAVA_BYTE, dstPos++, (byte) '0');
                        dst.set(JAVA_BYTE, dstPos++, (byte) '0');
                        dst.set(JAVA_BYTE, dstPos++, HEX_DIGITS[u >>> 4]);
                        dst.set(JAVA_BYTE, dstPos++, HEX_DIGITS[u & 0x0F]);
                    } else {
                        dst.set(JAVA_BYTE, dstPos++, (byte) u);
                    }
                }
            }
        }
        return dstPos;
    }

    // =========================================================================
    // Zero-Allocation POST /posts JSON Body Parser ({"body": "..."}, 1..500 chars)
    // =========================================================================

    /**
     * Parses and validates a POST /posts JSON request body: {"body": "..."}.
     * Per SPEC.md:
     *   - Must be a valid JSON object containing string field "body".
     *   - After trimming whitespace, must be non-empty, and length <= 500 Unicode characters.
     *
     * @return Packed slice handle (> 0; bit 62 = 1 if in unescapeScratch, 0 if in reqSeg),
     *         or -1L (malformed JSON), -2L (body is required), -3L (body > 500 chars).
     */
    public static long parsePostBody(
            MemorySegment reqSeg, long bodyOff, int bodyLen, MemorySegment unescapeScratch) {

        int i = 0;
        while (i < bodyLen && isJsonWs(reqSeg.get(JAVA_BYTE, bodyOff + i))) i++;
        int end = bodyLen - 1;
        while (end > i && isJsonWs(reqSeg.get(JAVA_BYTE, bodyOff + end))) end--;

        if (end - i < 1 ||
            reqSeg.get(JAVA_BYTE, bodyOff + i)   != '{' ||
            reqSeg.get(JAVA_BYTE, bodyOff + end) != '}') {
            return -1L;
        }
        i++;

        long resultHandle = -2L; // default if "body" is absent in a valid empty/other-key object
        boolean seenBodyKey = false;
        boolean expectKeyOrEnd = true;

        while (i < end) {
            while (i < end && isJsonWs(reqSeg.get(JAVA_BYTE, bodyOff + i))) i++;
            if (i >= end) break;

            if (!expectKeyOrEnd) {
                if (reqSeg.get(JAVA_BYTE, bodyOff + i) != ',') return -1L;
                i++;
                while (i < end && isJsonWs(reqSeg.get(JAVA_BYTE, bodyOff + i))) i++;
                if (i >= end) return -1L;
            }
            expectKeyOrEnd = false;

            if (reqSeg.get(JAVA_BYTE, bodyOff + i) != '"') return -1L;
            int keyStart = ++i;
            while (i < end) {
                byte b = reqSeg.get(JAVA_BYTE, bodyOff + i);
                if (b == '"') break;
                if (b == '\\') i++;
                if ((b & 0xFF) < 0x20) return -1L;
                i++;
            }
            if (i >= end) return -1L;
            int keyLen = i - keyStart;
            i++; // skip closing '"'

            while (i < end && isJsonWs(reqSeg.get(JAVA_BYTE, bodyOff + i))) i++;
            if (i >= end || reqSeg.get(JAVA_BYTE, bodyOff + i) != ':') return -1L;
            i++;
            while (i < end && isJsonWs(reqSeg.get(JAVA_BYTE, bodyOff + i))) i++;
            if (i >= end) return -1L;

            boolean isBodyKey = (keyLen == 4 &&
                reqSeg.get(JAVA_BYTE, bodyOff + keyStart)     == 'b' &&
                reqSeg.get(JAVA_BYTE, bodyOff + keyStart + 1) == 'o' &&
                reqSeg.get(JAVA_BYTE, bodyOff + keyStart + 2) == 'd' &&
                reqSeg.get(JAVA_BYTE, bodyOff + keyStart + 3) == 'y');

            if (isBodyKey) {
                if (seenBodyKey) return -1L;
                seenBodyKey = true;

                if (reqSeg.get(JAVA_BYTE, bodyOff + i) != '"') {
                    // Valid non-string JSON value (e.g. number, null, object, array) -> "body is required"
                    i = skipJsonValue(reqSeg, bodyOff, i, end);
                    if (i < 0) return -1L;
                    resultHandle = -2L;
                    continue;
                }
                i++;
                int valStart = i;
                boolean hasEscape = false;
                while (i < end) {
                    byte b = reqSeg.get(JAVA_BYTE, bodyOff + i);
                    if (b == '"') break;
                    if ((b & 0xFF) < 0x20) return -1L;
                    if (b == '\\') {
                        hasEscape = true;
                        i++;
                        if (i >= end) return -1L;
                    }
                    i++;
                }
                if (i >= end) return -1L;
                int valByteLen = i - valStart;
                i++; // skip closing '"'

                MemorySegment valSeg;
                long valOff;
                int valLen;
                if (!hasEscape) {
                    valSeg = reqSeg;
                    valOff = bodyOff + valStart;
                    valLen = valByteLen;
                } else {
                    int unescapedLen = unescapeJsonString(reqSeg, bodyOff + valStart, valByteLen, unescapeScratch);
                    if (unescapedLen < 0) return -1L;
                    valSeg = unescapeScratch;
                    valOff = 0L;
                    valLen = unescapedLen;
                }

                // Trim leading and trailing ASCII whitespace
                while (valLen > 0 && isJsonWs(valSeg.get(JAVA_BYTE, valOff))) {
                    valOff++;
                    valLen--;
                }
                while (valLen > 0 && isJsonWs(valSeg.get(JAVA_BYTE, valOff + valLen - 1))) {
                    valLen--;
                }

                if (valLen <= 0) {
                    resultHandle = -2L;
                } else {
                    int cpStatus = countValidCodePoints(valSeg, valOff, valLen);
                    if (cpStatus < 0) {
                        return -1L;
                    } else if (cpStatus > 500) {
                        resultHandle = -3L;
                    } else if (valSeg == unescapeScratch) {
                        resultHandle = (1L << 62) | (valOff << 32) | (valLen & 0xFFFFFFFFL);
                    } else {
                        resultHandle = (valOff << 32) | (valLen & 0xFFFFFFFFL);
                    }
                }
            } else {
                i = skipJsonValue(reqSeg, bodyOff, i, end);
                if (i < 0) return -1L;
            }
        }

        return resultHandle;
    }

    public static boolean isUnescapedInScratch(long handle) {
        return (handle & (1L << 62)) != 0L;
    }

    public static long sliceOffset(long handle) {
        return (handle >>> 32) & 0x3FFFFFFFL;
    }

    public static int sliceLength(long handle) {
        return (int) handle;
    }

    private static int countValidCodePoints(MemorySegment seg, long off, int byteLen) {
        int codePoints = 0;
        int i = 0;
        while (i < byteLen) {
            int b0 = seg.get(JAVA_BYTE, off + i) & 0xFF;
            if (b0 < 0x80) {
                i += 1;
            } else if ((b0 & 0xE0) == 0xC0) {
                if (i + 1 >= byteLen || (seg.get(JAVA_BYTE, off + i + 1) & 0xC0) != 0x80) return -1;
                i += 2;
            } else if ((b0 & 0xF0) == 0xE0) {
                if (i + 2 >= byteLen ||
                    (seg.get(JAVA_BYTE, off + i + 1) & 0xC0) != 0x80 ||
                    (seg.get(JAVA_BYTE, off + i + 2) & 0xC0) != 0x80) return -1;
                i += 3;
            } else if ((b0 & 0xF8) == 0xF0) {
                if (i + 3 >= byteLen ||
                    (seg.get(JAVA_BYTE, off + i + 1) & 0xC0) != 0x80 ||
                    (seg.get(JAVA_BYTE, off + i + 2) & 0xC0) != 0x80 ||
                    (seg.get(JAVA_BYTE, off + i + 3) & 0xC0) != 0x80) return -1;
                i += 4;
            } else {
                return -1;
            }
            codePoints++;
        }
        return codePoints;
    }

    private static int unescapeJsonString(
            MemorySegment src, long srcOff, int srcLen, MemorySegment dst) {
        int out = 0;
        int maxOut = (int) dst.byteSize();
        int i = 0;
        while (i < srcLen) {
            if (out + 4 > maxOut) return -1;
            byte b = src.get(JAVA_BYTE, srcOff + i++);
            if (b != '\\') {
                dst.set(JAVA_BYTE, out++, b);
                continue;
            }
            if (i >= srcLen) return -1;
            byte esc = src.get(JAVA_BYTE, srcOff + i++);
            switch (esc) {
                case '"', '\\', '/' -> dst.set(JAVA_BYTE, out++, esc);
                case 'b' -> dst.set(JAVA_BYTE, out++, (byte) '\b');
                case 'f' -> dst.set(JAVA_BYTE, out++, (byte) '\f');
                case 'n' -> dst.set(JAVA_BYTE, out++, (byte) '\n');
                case 'r' -> dst.set(JAVA_BYTE, out++, (byte) '\r');
                case 't' -> dst.set(JAVA_BYTE, out++, (byte) '\t');
                case 'u' -> {
                    if (i + 4 > srcLen) return -1;
                    int cp = parseHex4(src, srcOff + i);
                    if (cp < 0) return -1;
                    i += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        if (i + 6 > srcLen ||
                            src.get(JAVA_BYTE, srcOff + i) != '\\' ||
                            src.get(JAVA_BYTE, srcOff + i + 1) != 'u') return -1;
                        int low = parseHex4(src, srcOff + i + 2);
                        if (low < 0xDC00 || low > 0xDFFF) return -1;
                        i += 6;
                        cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        return -1;
                    }
                    if (cp < 0x80) {
                        dst.set(JAVA_BYTE, out++, (byte) cp);
                    } else if (cp < 0x800) {
                        dst.set(JAVA_BYTE, out++, (byte) (0xC0 | (cp >>> 6)));
                        dst.set(JAVA_BYTE, out++, (byte) (0x80 | (cp & 0x3F)));
                    } else if (cp < 0x10000) {
                        dst.set(JAVA_BYTE, out++, (byte) (0xE0 | (cp >>> 12)));
                        dst.set(JAVA_BYTE, out++, (byte) (0x80 | ((cp >>> 6) & 0x3F)));
                        dst.set(JAVA_BYTE, out++, (byte) (0x80 | (cp & 0x3F)));
                    } else {
                        dst.set(JAVA_BYTE, out++, (byte) (0xF0 | (cp >>> 18)));
                        dst.set(JAVA_BYTE, out++, (byte) (0x80 | ((cp >>> 12) & 0x3F)));
                        dst.set(JAVA_BYTE, out++, (byte) (0x80 | ((cp >>> 6) & 0x3F)));
                        dst.set(JAVA_BYTE, out++, (byte) (0x80 | (cp & 0x3F)));
                    }
                }
                default -> {
                    return -1;
                }
            }
        }
        return out;
    }

    private static int parseHex4(MemorySegment seg, long off) {
        int val = 0;
        for (int k = 0; k < 4; k++) {
            int c = seg.get(JAVA_BYTE, off + k) & 0xFF;
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return -1;
            val = (val << 4) | d;
        }
        return val;
    }

    private static int skipJsonValue(MemorySegment seg, long baseOff, int i, int end) {
        byte b = seg.get(JAVA_BYTE, baseOff + i);
        if (b == '"') {
            i++;
            while (i < end) {
                byte c = seg.get(JAVA_BYTE, baseOff + i++);
                if (c == '"') return i;
                if (c == '\\') i++;
                if ((c & 0xFF) < 0x20) return -1;
            }
            return -1;
        } else if (b == '{' || b == '[') {
            int depth = 1;
            i++;
            while (i < end && depth > 0) {
                byte c = seg.get(JAVA_BYTE, baseOff + i++);
                if (c == '"') {
                    while (i < end) {
                        byte sc = seg.get(JAVA_BYTE, baseOff + i++);
                        if (sc == '"') break;
                        if (sc == '\\') i++;
                    }
                } else if (c == '{' || c == '[') {
                    depth++;
                } else if (c == '}' || c == ']') {
                    depth--;
                }
            }
            return depth == 0 ? i : -1;
        } else {
            int start = i;
            while (i < end) {
                byte c = seg.get(JAVA_BYTE, baseOff + i);
                if (c == ',' || isJsonWs(c)) break;
                i++;
            }
            return i > start ? i : -1;
        }
    }

    private static boolean isJsonWs(byte b) {
        return b == ' ' || b == '\t' || b == '\r' || b == '\n';
    }

    // =========================================================================
    // Fast Branchless 2-Digit LUT Integer-to-ASCII (itoa)
    // =========================================================================

    public static long writeIntAscii(MemorySegment dst, long pos, int val) {
        if (val < 10) {
            dst.set(JAVA_BYTE, pos, (byte) ('0' + val));
            return pos + 1L;
        }
        if (val < 100) {
            dst.set(JAVA_SHORT_UNALIGNED, pos, DIGIT_PAIRS[val]);
            return pos + 2L;
        }
        if (val < 1000) {
            int q = (val * 5243) >>> 19;
            int r = val - q * 100;
            dst.set(JAVA_BYTE, pos, (byte) ('0' + q));
            dst.set(JAVA_SHORT_UNALIGNED, pos + 1L, DIGIT_PAIRS[r]);
            return pos + 3L;
        }
        if (val < 10000) {
            int q = (val * 5243) >>> 19;
            int r = val - q * 100;
            dst.set(JAVA_SHORT_UNALIGNED, pos, DIGIT_PAIRS[q]);
            dst.set(JAVA_SHORT_UNALIGNED, pos + 2L, DIGIT_PAIRS[r]);
            return pos + 4L;
        }
        return writeLongAscii(dst, pos, Integer.toUnsignedLong(val));
    }

    public static long writeLongAscii(MemorySegment dst, long pos, long val) {
        if (val == 0L) {
            dst.set(JAVA_BYTE, pos, (byte) '0');
            return pos + 1L;
        }
        int digits = decimalLength(val);
        long endPos = pos + digits;
        long cur = endPos;
        while (val >= 100L) {
            long q = val / 100L;
            int r = (int) (val - q * 100L);
            cur -= 2L;
            dst.set(JAVA_SHORT_UNALIGNED, cur, DIGIT_PAIRS[r]);
            val = q;
        }
        if (val < 10L) {
            dst.set(JAVA_BYTE, pos, (byte) ('0' + val));
        } else {
            dst.set(JAVA_SHORT_UNALIGNED, pos, DIGIT_PAIRS[(int) val]);
        }
        return endPos;
    }

    private static int decimalLength(long v) {
        if (v < 10L) return 1;
        if (v < 100L) return 2;
        if (v < 1000L) return 3;
        if (v < 10000L) return 4;
        if (v < 100000L) return 5;
        if (v < 1000000L) return 6;
        if (v < 10000000L) return 7;
        if (v < 100000000L) return 8;
        if (v < 1000000000L) return 9;
        if (v < 10000000000L) return 10;
        if (v < 100000000000L) return 11;
        if (v < 1000000000000L) return 12;
        if (v < 10000000000000L) return 13;
        if (v < 100000000000000L) return 14;
        if (v < 1000000000000000L) return 15;
        if (v < 10000000000000000L) return 16;
        if (v < 100000000000000000L) return 17;
        if (v < 1000000000000000000L) return 18;
        return 19;
    }

    private static void selfTest() {
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment pgSim = arena.allocate(256L);
            byte[] raw = "500000|Hello \"world\"|2025-12-31 23:59:19.4+00|user_123|7"
                    .getBytes(StandardCharsets.UTF_8);
            MemorySegment.copy(raw, 0, pgSim, JAVA_BYTE, 0L, raw.length);

            MemorySegment out = arena.allocate(512L);
            long p = startSinglePostObject(out, 0L);
            p = writePostFromPgText(
                out, p, false, pgSim,
                0L, 6,
                7L, 13,
                21L, 24,
                46L, 8,
                55L, 1
            );
            p = endSinglePostObject(out, p);
            String json = new String(out.asSlice(0L, p).toArray(JAVA_BYTE), StandardCharsets.UTF_8);
            String expected = "{\"post\":{\"id\":500000,\"body\":\"Hello \\\"world\\\"\",\"created_at\":\"2025-12-31T23:59:19.400Z\",\"author\":\"user_123\",\"like_count\":7}}";
            if (!json.equals(expected)) {
                throw new AssertionError("Json selfTest mismatch:\nGot: " + json + "\nExp: " + expected);
            }
        }
    }
}
