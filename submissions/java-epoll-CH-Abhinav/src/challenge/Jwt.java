package challenge;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Arrays;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT_UNALIGNED;
import static java.lang.foreign.ValueLayout.JAVA_LONG_UNALIGNED;
import static java.lang.foreign.ValueLayout.JAVA_SHORT_UNALIGNED;

public final class Jwt {

    private Jwt() {}

    // Scratch layout (512 bytes per worker):
    //   0..31  : SHA-256 working state H0..H7 (32B)
    //  32..35  : Decoded username offset inside scratch (int)
    //  36..39  : Decoded username byte length (int)
    //  64..127 : SHA-256 tail padding block (64B)
    // 128..511 : Decoded JWT payload / header bytes (384B)
    public static final long SCRATCH_BYTES    = 512L;
    private static final long USERNAME_OFF_POS = 32L;
    private static final long USERNAME_LEN_POS = 36L;
    private static final long TAIL_BLOCK_OFF   = 64L;
    private static final long DECODED_OFF      = 128L;
    private static final int  MAX_B64_LEN      = 500;

    // Canonical Base64Url for {"alg":"HS256","typ":"JWT"} (36 bytes)
    private static final long HDR_W0 = 0x69634762684A7965L;
    private static final long HDR_W1 = 0x31497A55494A694FL;
    private static final long HDR_W2 = 0x35526E497349694EL;
    private static final long HDR_W3 = 0x58706B4936494363L;
    private static final int  HDR_W4 = 0x394A4356;

    // Precomputed SHA-256 midstates for (K ^ ipad) and (K ^ opad)
    private static int ipad0, ipad1, ipad2, ipad3, ipad4, ipad5, ipad6, ipad7;
    private static int opad0, opad1, opad2, opad3, opad4, opad5, opad6, opad7;

    private static final byte[]  B64_CHAR    = new byte[64];
    private static final short[] B64_PAIR_LE = new short[4096];
    private static final byte[]  B64_DEC     = new byte[256];

    static {
        byte[] alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                .getBytes(StandardCharsets.US_ASCII);
        System.arraycopy(alphabet, 0, B64_CHAR, 0, 64);

        for (int i = 0; i < 4096; i++) {
            int c0 = alphabet[(i >>> 6) & 0x3F] & 0xFF;
            int c1 = alphabet[i & 0x3F] & 0xFF;
            B64_PAIR_LE[i] = (short) (c0 | (c1 << 8));
        }

        Arrays.fill(B64_DEC, (byte) -1);
        for (int i = 0; i < 64; i++) {
            B64_DEC[alphabet[i] & 0xFF] = (byte) i;
        }
    }

    public static void init(String secret) {
        try {
            byte[] keyBytes = secret.getBytes(StandardCharsets.UTF_8);
            if (keyBytes.length > 64) {
                keyBytes = MessageDigest.getInstance("SHA-256").digest(keyBytes);
            }
            byte[] kPad = Arrays.copyOf(keyBytes, 64);

            try (Arena initArena = Arena.ofConfined()) {
                MemorySegment block = initArena.allocate(64L, 8L);
                MemorySegment state = initArena.allocate(32L, 8L);

                for (int i = 0; i < 64; i++) {
                    block.set(JAVA_BYTE, i, (byte) (kPad[i] ^ 0x36));
                }
                initSha256Iv(state);
                compressBlock(block, 0L, state);
                ipad0 = state.get(JAVA_INT_UNALIGNED, 0L);
                ipad1 = state.get(JAVA_INT_UNALIGNED, 4L);
                ipad2 = state.get(JAVA_INT_UNALIGNED, 8L);
                ipad3 = state.get(JAVA_INT_UNALIGNED, 12L);
                ipad4 = state.get(JAVA_INT_UNALIGNED, 16L);
                ipad5 = state.get(JAVA_INT_UNALIGNED, 20L);
                ipad6 = state.get(JAVA_INT_UNALIGNED, 24L);
                ipad7 = state.get(JAVA_INT_UNALIGNED, 28L);

                for (int i = 0; i < 64; i++) {
                    block.set(JAVA_BYTE, i, (byte) (kPad[i] ^ 0x5C));
                }
                initSha256Iv(state);
                compressBlock(block, 0L, state);
                opad0 = state.get(JAVA_INT_UNALIGNED, 0L);
                opad1 = state.get(JAVA_INT_UNALIGNED, 4L);
                opad2 = state.get(JAVA_INT_UNALIGNED, 8L);
                opad3 = state.get(JAVA_INT_UNALIGNED, 12L);
                opad4 = state.get(JAVA_INT_UNALIGNED, 16L);
                opad5 = state.get(JAVA_INT_UNALIGNED, 20L);
                opad6 = state.get(JAVA_INT_UNALIGNED, 24L);
                opad7 = state.get(JAVA_INT_UNALIGNED, 28L);

                selfTest(secret, initArena);
            }
        } catch (Exception e) {
            throw new IllegalStateException("Failed to initialize Jwt midstates", e);
        }
    }

    private static void initSha256Iv(MemorySegment state) {
        state.set(JAVA_INT_UNALIGNED, 0L,  0x6a09e667);
        state.set(JAVA_INT_UNALIGNED, 4L,  0xbb67ae85);
        state.set(JAVA_INT_UNALIGNED, 8L,  0x3c6ef372);
        state.set(JAVA_INT_UNALIGNED, 12L, 0xa54ff53a);
        state.set(JAVA_INT_UNALIGNED, 16L, 0x510e527f);
        state.set(JAVA_INT_UNALIGNED, 20L, 0x9b05688c);
        state.set(JAVA_INT_UNALIGNED, 24L, 0x1f83d9ab);
        state.set(JAVA_INT_UNALIGNED, 28L, 0x5be0cd19);
    }

    /**
     * Returns the offset inside `scratch` where the verified token's `username` string begins.
     */
    public static long usernameOffset(MemorySegment scratch) {
        return scratch.get(JAVA_INT_UNALIGNED, USERNAME_OFF_POS);
    }

    /**
     * Returns the byte length of the verified token's `username` string inside `scratch`.
     */
    public static int usernameLength(MemorySegment scratch) {
        return scratch.get(JAVA_INT_UNALIGNED, USERNAME_LEN_POS);
    }

    public static long verifyAuthHeader(
            MemorySegment reqSeg, long valOff, int valLen,
            long nowEpochSec, MemorySegment scratch) {

        while (valLen > 0) {
            byte b = reqSeg.get(JAVA_BYTE, valOff);
            if (b != ' ' && b != '\t') break;
            valOff++;
            valLen--;
        }
        while (valLen > 0) {
            byte b = reqSeg.get(JAVA_BYTE, valOff + valLen - 1);
            if (b != ' ' && b != '\t') break;
            valLen--;
        }
        if (valLen < 50) return -1L;
        if ((reqSeg.get(JAVA_BYTE, valOff)     | 0x20) != 'b' ||
            (reqSeg.get(JAVA_BYTE, valOff + 1) | 0x20) != 'e' ||
            (reqSeg.get(JAVA_BYTE, valOff + 2) | 0x20) != 'a' ||
            (reqSeg.get(JAVA_BYTE, valOff + 3) | 0x20) != 'r' ||
            (reqSeg.get(JAVA_BYTE, valOff + 4) | 0x20) != 'e' ||
            (reqSeg.get(JAVA_BYTE, valOff + 5) | 0x20) != 'r' ||
             reqSeg.get(JAVA_BYTE, valOff + 6) != ' ') {
            return -1L;
        }
        valOff += 7;
        valLen -= 7;
        while (valLen > 0 && reqSeg.get(JAVA_BYTE, valOff) == ' ') {
            valOff++;
            valLen--;
        }
        return verifyToken(reqSeg, valOff, valLen, nowEpochSec, scratch);
    }

    public static long verifyToken(
            MemorySegment reqSeg, long tokenOff, int tokenLen,
            long nowEpochSec, MemorySegment scratch) {

        if (tokenLen < 50 || tokenLen > 1024) {
            return -1L;
        }

        int dot2 = tokenLen - 44;
        if (reqSeg.get(JAVA_BYTE, tokenOff + dot2) != '.') {
            return -1L;
        }

        int dot1 = -1;
        if (reqSeg.get(JAVA_BYTE, tokenOff + 36) == '.') {
            dot1 = 36;
        } else {
            for (int i = 1; i < dot2; i++) {
                if (reqSeg.get(JAVA_BYTE, tokenOff + i) == '.') {
                    dot1 = i;
                    break;
                }
            }
            if (dot1 <= 0) return -1L;
        }

        for (int i = dot1 + 1; i < dot2; i++) {
            if (reqSeg.get(JAVA_BYTE, tokenOff + i) == '.') {
                return -1L;
            }
        }

        if (dot1 == 36 &&
            reqSeg.get(JAVA_LONG_UNALIGNED, tokenOff)       == HDR_W0 &&
            reqSeg.get(JAVA_LONG_UNALIGNED, tokenOff + 8L)  == HDR_W1 &&
            reqSeg.get(JAVA_LONG_UNALIGNED, tokenOff + 16L) == HDR_W2 &&
            reqSeg.get(JAVA_LONG_UNALIGNED, tokenOff + 24L) == HDR_W3 &&
            reqSeg.get(JAVA_INT_UNALIGNED,  tokenOff + 32L) == HDR_W4) {
            // Fast path matched {"alg":"HS256","typ":"JWT"}
        } else if (!verifyHeaderSlow(reqSeg, tokenOff, dot1, scratch)) {
            return -1L;
        }

        // Load precomputed IPAD midstate
        scratch.set(JAVA_INT_UNALIGNED, 0L,  ipad0);
        scratch.set(JAVA_INT_UNALIGNED, 4L,  ipad1);
        scratch.set(JAVA_INT_UNALIGNED, 8L,  ipad2);
        scratch.set(JAVA_INT_UNALIGNED, 12L, ipad3);
        scratch.set(JAVA_INT_UNALIGNED, 16L, ipad4);
        scratch.set(JAVA_INT_UNALIGNED, 20L, ipad5);
        scratch.set(JAVA_INT_UNALIGNED, 24L, ipad6);
        scratch.set(JAVA_INT_UNALIGNED, 28L, ipad7);

        int fullEnd = dot2 & ~63;
        for (int off = 0; off < fullEnd; off += 64) {
            compressBlock(reqSeg, tokenOff + off, scratch);
        }

        int rem = dot2 - fullEnd;
        zero64B(scratch, TAIL_BLOCK_OFF);
        copyBytes(reqSeg, tokenOff + fullEnd, scratch, TAIL_BLOCK_OFF, rem);
        scratch.set(JAVA_BYTE, TAIL_BLOCK_OFF + rem, (byte) 0x80);

        if (rem >= 56) {
            compressBlock(scratch, TAIL_BLOCK_OFF, scratch);
            zero64B(scratch, TAIL_BLOCK_OFF);
        }
        long totalInnerBits = (64L + dot2) << 3;
        scratch.set(JAVA_LONG_UNALIGNED, TAIL_BLOCK_OFF + 56L, Long.reverseBytes(totalInnerBits));
        compressBlock(scratch, TAIL_BLOCK_OFF, scratch);

        long sigOff = tokenOff + dot2 + 1;
        if (!computeOuterAndCompareSig(scratch, reqSeg, sigOff)) {
            return -1L;
        }

        long payB64Off = tokenOff + dot1 + 1;
        int payB64Len = dot2 - (dot1 + 1);
        int payLen = decodeBase64Url(reqSeg, payB64Off, payB64Len, scratch, DECODED_OFF);
        if (payLen <= 0) {
            return -1L;
        }

        return parseAndValidateClaims(scratch, DECODED_OFF, payLen, nowEpochSec);
    }

    private static boolean computeOuterAndCompareSig(
            MemorySegment state, MemorySegment reqSeg, long sigOff) {

        int w0 = state.get(JAVA_INT_UNALIGNED, 0L);
        int w1 = state.get(JAVA_INT_UNALIGNED, 4L);
        int w2 = state.get(JAVA_INT_UNALIGNED, 8L);
        int w3 = state.get(JAVA_INT_UNALIGNED, 12L);
        int w4 = state.get(JAVA_INT_UNALIGNED, 16L);
        int w5 = state.get(JAVA_INT_UNALIGNED, 20L);
        int w6 = state.get(JAVA_INT_UNALIGNED, 24L);
        int w7 = state.get(JAVA_INT_UNALIGNED, 28L);
        int w8 = 0x80000000;
        int w9 = 0, w10 = 0, w11 = 0, w12 = 0, w13 = 0, w14 = 0;
        int w15 = 768;

        int a = opad0, b = opad1, c = opad2, d = opad3;
        int e = opad4, f = opad5, g = opad6, h = opad7;
        int t1, t2;

        // Rounds 0..15
        t1 = h + S1(e) + ch(e, f, g) + 0x428a2f98 + w0;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0x71374491 + w1;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0xb5c0fbcf + w2;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0xe9b5dba5 + w3;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0x3956c25b + w4;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0x59f111f1 + w5;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0x923f82a4 + w6;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0xab1c5ed5 + w7;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0xd807aa98 + w8;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0x12835b01;       t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0x243185be;       t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0x550c7dc3;       t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0x72be5d74;       t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0x80deb1fe;       t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0x9bdc06a7;       t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0xc19bf174 + w15; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;

        // Rounds 16..31
        w0  += s0(w1)  + w9  + s1(w14); t1 = h + S1(e) + ch(e, f, g) + 0xe49b69c1 + w0;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w1  += s0(w2)  + w10 + s1(w15); t1 = h + S1(e) + ch(e, f, g) + 0xefbe4786 + w1;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w2  += s0(w3)  + w11 + s1(w0);  t1 = h + S1(e) + ch(e, f, g) + 0x0fc19dc6 + w2;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w3  += s0(w4)  + w12 + s1(w1);  t1 = h + S1(e) + ch(e, f, g) + 0x240ca1cc + w3;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w4  += s0(w5)  + w13 + s1(w2);  t1 = h + S1(e) + ch(e, f, g) + 0x2de92c6f + w4;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w5  += s0(w6)  + w14 + s1(w3);  t1 = h + S1(e) + ch(e, f, g) + 0x4a7484aa + w5;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w6  += s0(w7)  + w15 + s1(w4);  t1 = h + S1(e) + ch(e, f, g) + 0x5cb0a9dc + w6;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w7  += s0(w8)  + w0  + s1(w5);  t1 = h + S1(e) + ch(e, f, g) + 0x76f988da + w7;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w8  += s0(w9)  + w1  + s1(w6);  t1 = h + S1(e) + ch(e, f, g) + 0x983e5152 + w8;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w9  += s0(w10) + w2  + s1(w7);  t1 = h + S1(e) + ch(e, f, g) + 0xa831c66d + w9;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w10 += s0(w11) + w3  + s1(w8);  t1 = h + S1(e) + ch(e, f, g) + 0xb00327c8 + w10; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w11 += s0(w12) + w4  + s1(w9);  t1 = h + S1(e) + ch(e, f, g) + 0xbf597fc7 + w11; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w12 += s0(w13) + w5  + s1(w10); t1 = h + S1(e) + ch(e, f, g) + 0xc6e00bf3 + w12; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w13 += s0(w14) + w6  + s1(w11); t1 = h + S1(e) + ch(e, f, g) + 0xd5a79147 + w13; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w14 += s0(w15) + w7  + s1(w12); t1 = h + S1(e) + ch(e, f, g) + 0x06ca6351 + w14; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w15 += s0(w0)  + w8  + s1(w13); t1 = h + S1(e) + ch(e, f, g) + 0x14292967 + w15; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;

        // Rounds 32..47
        w0  += s0(w1)  + w9  + s1(w14); t1 = h + S1(e) + ch(e, f, g) + 0x27b70a85 + w0;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w1  += s0(w2)  + w10 + s1(w15); t1 = h + S1(e) + ch(e, f, g) + 0x2e1b2138 + w1;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w2  += s0(w3)  + w11 + s1(w0);  t1 = h + S1(e) + ch(e, f, g) + 0x4d2c6dfc + w2;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w3  += s0(w4)  + w12 + s1(w1);  t1 = h + S1(e) + ch(e, f, g) + 0x53380d13 + w3;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w4  += s0(w5)  + w13 + s1(w2);  t1 = h + S1(e) + ch(e, f, g) + 0x650a7354 + w4;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w5  += s0(w6)  + w14 + s1(w3);  t1 = h + S1(e) + ch(e, f, g) + 0x766a0abb + w5;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w6  += s0(w7)  + w15 + s1(w4);  t1 = h + S1(e) + ch(e, f, g) + 0x81c2c92e + w6;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w7  += s0(w8)  + w0  + s1(w5);  t1 = h + S1(e) + ch(e, f, g) + 0x92722c85 + w7;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w8  += s0(w9)  + w1  + s1(w6);  t1 = h + S1(e) + ch(e, f, g) + 0xa2bfe8a1 + w8;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w9  += s0(w10) + w2  + s1(w7);  t1 = h + S1(e) + ch(e, f, g) + 0xa81a664b + w9;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w10 += s0(w11) + w3  + s1(w8);  t1 = h + S1(e) + ch(e, f, g) + 0xc24b8b70 + w10; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w11 += s0(w12) + w4  + s1(w9);  t1 = h + S1(e) + ch(e, f, g) + 0xc76c51a3 + w11; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w12 += s0(w13) + w5  + s1(w10); t1 = h + S1(e) + ch(e, f, g) + 0xd192e819 + w12; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w13 += s0(w14) + w6  + s1(w11); t1 = h + S1(e) + ch(e, f, g) + 0xd6990624 + w13; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w14 += s0(w15) + w7  + s1(w12); t1 = h + S1(e) + ch(e, f, g) + 0xf40e3585 + w14; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w15 += s0(w0)  + w8  + s1(w13); t1 = h + S1(e) + ch(e, f, g) + 0x106aa070 + w15; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;

        // Rounds 48..63
        w0  += s0(w1)  + w9  + s1(w14); t1 = h + S1(e) + ch(e, f, g) + 0x19a4c116 + w0;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w1  += s0(w2)  + w10 + s1(w15); t1 = h + S1(e) + ch(e, f, g) + 0x1e376c08 + w1;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w2  += s0(w3)  + w11 + s1(w0);  t1 = h + S1(e) + ch(e, f, g) + 0x2748774c + w2;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w3  += s0(w4)  + w12 + s1(w1);  t1 = h + S1(e) + ch(e, f, g) + 0x34b0bcb5 + w3;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w4  += s0(w5)  + w13 + s1(w2);  t1 = h + S1(e) + ch(e, f, g) + 0x391c0cb3 + w4;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w5  += s0(w6)  + w14 + s1(w3);  t1 = h + S1(e) + ch(e, f, g) + 0x4ed8aa4a + w5;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w6  += s0(w7)  + w15 + s1(w4);  t1 = h + S1(e) + ch(e, f, g) + 0x5b9cca4f + w6;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w7  += s0(w8)  + w0  + s1(w5);  t1 = h + S1(e) + ch(e, f, g) + 0x682e6ff3 + w7;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w8  += s0(w9)  + w1  + s1(w6);  t1 = h + S1(e) + ch(e, f, g) + 0x748f82ee + w8;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w9  += s0(w10) + w2  + s1(w7);  t1 = h + S1(e) + ch(e, f, g) + 0x78a5636f + w9;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w10 += s0(w11) + w3  + s1(w8);  t1 = h + S1(e) + ch(e, f, g) + 0x84c87814 + w10; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w11 += s0(w12) + w4  + s1(w9);  t1 = h + S1(e) + ch(e, f, g) + 0x8cc70208 + w11; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w12 += s0(w13) + w5  + s1(w10); t1 = h + S1(e) + ch(e, f, g) + 0x90befffa + w12; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w13 += s0(w14) + w6  + s1(w11); t1 = h + S1(e) + ch(e, f, g) + 0xa4506ceb + w13; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w14 += s0(w15) + w7  + s1(w12); t1 = h + S1(e) + ch(e, f, g) + 0xbef9a3f7 + w14; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w15 += s0(w0)  + w8  + s1(w13); t1 = h + S1(e) + ch(e, f, g) + 0xc67178f2 + w15; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;

        int o0 = opad0 + a;
        int o1 = opad1 + b;
        int o2 = opad2 + c;
        int o3 = opad3 + d;
        int o4 = opad4 + e;
        int o5 = opad5 + f;
        int o6 = opad6 + g;
        int o7 = opad7 + h;

        long d0 = (Integer.toUnsignedLong(o0) << 32) | Integer.toUnsignedLong(o1);
        long d1 = (Integer.toUnsignedLong(o2) << 32) | Integer.toUnsignedLong(o3);
        long d2 = (Integer.toUnsignedLong(o4) << 32) | Integer.toUnsignedLong(o5);
        long d3 = (Integer.toUnsignedLong(o6) << 32) | Integer.toUnsignedLong(o7);

        long exp0 = encode48To64LE(d0 >>> 16);
        long exp1 = encode48To64LE(((d0 & 0xFFFFL) << 32) | (d1 >>> 32));
        long exp2 = encode48To64LE(((d1 & 0xFFFFFFFFL) << 16) | (d2 >>> 48));
        long exp3 = encode48To64LE(d2 & 0xFFFFFFFFFFFFL);
        long exp4 = encode48To64LE(d3 >>> 16);

        int v18   = ((int) (d3 & 0xFFFFL)) << 2;
        int exp40 = B64_PAIR_LE[(v18 >>> 6) & 0xFFF] & 0xFFFF;
        int exp42 = B64_CHAR[v18 & 0x3F] & 0xFF;

        long diff = (reqSeg.get(JAVA_LONG_UNALIGNED, sigOff)       ^ exp0)
                  | (reqSeg.get(JAVA_LONG_UNALIGNED, sigOff + 8L)  ^ exp1)
                  | (reqSeg.get(JAVA_LONG_UNALIGNED, sigOff + 16L) ^ exp2)
                  | (reqSeg.get(JAVA_LONG_UNALIGNED, sigOff + 24L) ^ exp3)
                  | (reqSeg.get(JAVA_LONG_UNALIGNED, sigOff + 32L) ^ exp4)
                  | ((reqSeg.get(JAVA_SHORT_UNALIGNED, sigOff + 40L) & 0xFFFF) ^ exp40)
                  | ((reqSeg.get(JAVA_BYTE, sigOff + 42L) & 0xFF) ^ exp42);

        return diff == 0L;
    }

    private static long encode48To64LE(long v48) {
        long p0 = B64_PAIR_LE[(int) ((v48 >>> 36) & 0xFFFL)] & 0xFFFFL;
        long p1 = B64_PAIR_LE[(int) ((v48 >>> 24) & 0xFFFL)] & 0xFFFFL;
        long p2 = B64_PAIR_LE[(int) ((v48 >>> 12) & 0xFFFL)] & 0xFFFFL;
        long p3 = B64_PAIR_LE[(int) (v48 & 0xFFFL)] & 0xFFFFL;
        return p0 | (p1 << 16) | (p2 << 32) | (p3 << 48);
    }

    public static void compressBlock(MemorySegment block, long off, MemorySegment state) {
        int w0  = Integer.reverseBytes(block.get(JAVA_INT_UNALIGNED, off));
        int w1  = Integer.reverseBytes(block.get(JAVA_INT_UNALIGNED, off + 4L));
        int w2  = Integer.reverseBytes(block.get(JAVA_INT_UNALIGNED, off + 8L));
        int w3  = Integer.reverseBytes(block.get(JAVA_INT_UNALIGNED, off + 12L));
        int w4  = Integer.reverseBytes(block.get(JAVA_INT_UNALIGNED, off + 16L));
        int w5  = Integer.reverseBytes(block.get(JAVA_INT_UNALIGNED, off + 20L));
        int w6  = Integer.reverseBytes(block.get(JAVA_INT_UNALIGNED, off + 24L));
        int w7  = Integer.reverseBytes(block.get(JAVA_INT_UNALIGNED, off + 28L));
        int w8  = Integer.reverseBytes(block.get(JAVA_INT_UNALIGNED, off + 32L));
        int w9  = Integer.reverseBytes(block.get(JAVA_INT_UNALIGNED, off + 36L));
        int w10 = Integer.reverseBytes(block.get(JAVA_INT_UNALIGNED, off + 40L));
        int w11 = Integer.reverseBytes(block.get(JAVA_INT_UNALIGNED, off + 44L));
        int w12 = Integer.reverseBytes(block.get(JAVA_INT_UNALIGNED, off + 48L));
        int w13 = Integer.reverseBytes(block.get(JAVA_INT_UNALIGNED, off + 52L));
        int w14 = Integer.reverseBytes(block.get(JAVA_INT_UNALIGNED, off + 56L));
        int w15 = Integer.reverseBytes(block.get(JAVA_INT_UNALIGNED, off + 60L));

        int h0 = state.get(JAVA_INT_UNALIGNED, 0L);
        int h1 = state.get(JAVA_INT_UNALIGNED, 4L);
        int h2 = state.get(JAVA_INT_UNALIGNED, 8L);
        int h3 = state.get(JAVA_INT_UNALIGNED, 12L);
        int h4 = state.get(JAVA_INT_UNALIGNED, 16L);
        int h5 = state.get(JAVA_INT_UNALIGNED, 20L);
        int h6 = state.get(JAVA_INT_UNALIGNED, 24L);
        int h7 = state.get(JAVA_INT_UNALIGNED, 28L);

        int a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
        int t1, t2;

        // Rounds 0..15
        t1 = h + S1(e) + ch(e, f, g) + 0x428a2f98 + w0;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0x71374491 + w1;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0xb5c0fbcf + w2;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0xe9b5dba5 + w3;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0x3956c25b + w4;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0x59f111f1 + w5;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0x923f82a4 + w6;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0xab1c5ed5 + w7;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0xd807aa98 + w8;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0x12835b01 + w9;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0x243185be + w10; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0x550c7dc3 + w11; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0x72be5d74 + w12; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0x80deb1fe + w13; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0x9bdc06a7 + w14; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        t1 = h + S1(e) + ch(e, f, g) + 0xc19bf174 + w15; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;

        // Rounds 16..31
        w0  += s0(w1)  + w9  + s1(w14); t1 = h + S1(e) + ch(e, f, g) + 0xe49b69c1 + w0;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w1  += s0(w2)  + w10 + s1(w15); t1 = h + S1(e) + ch(e, f, g) + 0xefbe4786 + w1;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w2  += s0(w3)  + w11 + s1(w0);  t1 = h + S1(e) + ch(e, f, g) + 0x0fc19dc6 + w2;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w3  += s0(w4)  + w12 + s1(w1);  t1 = h + S1(e) + ch(e, f, g) + 0x240ca1cc + w3;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w4  += s0(w5)  + w13 + s1(w2);  t1 = h + S1(e) + ch(e, f, g) + 0x2de92c6f + w4;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w5  += s0(w6)  + w14 + s1(w3);  t1 = h + S1(e) + ch(e, f, g) + 0x4a7484aa + w5;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w6  += s0(w7)  + w15 + s1(w4);  t1 = h + S1(e) + ch(e, f, g) + 0x5cb0a9dc + w6;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w7  += s0(w8)  + w0  + s1(w5);  t1 = h + S1(e) + ch(e, f, g) + 0x76f988da + w7;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w8  += s0(w9)  + w1  + s1(w6);  t1 = h + S1(e) + ch(e, f, g) + 0x983e5152 + w8;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w9  += s0(w10) + w2  + s1(w7);  t1 = h + S1(e) + ch(e, f, g) + 0xa831c66d + w9;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w10 += s0(w11) + w3  + s1(w8);  t1 = h + S1(e) + ch(e, f, g) + 0xb00327c8 + w10; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w11 += s0(w12) + w4  + s1(w9);  t1 = h + S1(e) + ch(e, f, g) + 0xbf597fc7 + w11; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w12 += s0(w13) + w5  + s1(w10); t1 = h + S1(e) + ch(e, f, g) + 0xc6e00bf3 + w12; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w13 += s0(w14) + w6  + s1(w11); t1 = h + S1(e) + ch(e, f, g) + 0xd5a79147 + w13; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w14 += s0(w15) + w7  + s1(w12); t1 = h + S1(e) + ch(e, f, g) + 0x06ca6351 + w14; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w15 += s0(w0)  + w8  + s1(w13); t1 = h + S1(e) + ch(e, f, g) + 0x14292967 + w15; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;

        // Rounds 32..47
        w0  += s0(w1)  + w9  + s1(w14); t1 = h + S1(e) + ch(e, f, g) + 0x27b70a85 + w0;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w1  += s0(w2)  + w10 + s1(w15); t1 = h + S1(e) + ch(e, f, g) + 0x2e1b2138 + w1;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w2  += s0(w3)  + w11 + s1(w0);  t1 = h + S1(e) + ch(e, f, g) + 0x4d2c6dfc + w2;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w3  += s0(w4)  + w12 + s1(w1);  t1 = h + S1(e) + ch(e, f, g) + 0x53380d13 + w3;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w4  += s0(w5)  + w13 + s1(w2);  t1 = h + S1(e) + ch(e, f, g) + 0x650a7354 + w4;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w5  += s0(w6)  + w14 + s1(w3);  t1 = h + S1(e) + ch(e, f, g) + 0x766a0abb + w5;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w6  += s0(w7)  + w15 + s1(w4);  t1 = h + S1(e) + ch(e, f, g) + 0x81c2c92e + w6;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w7  += s0(w8)  + w0  + s1(w5);  t1 = h + S1(e) + ch(e, f, g) + 0x92722c85 + w7;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w8  += s0(w9)  + w1  + s1(w6);  t1 = h + S1(e) + ch(e, f, g) + 0xa2bfe8a1 + w8;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w9  += s0(w10) + w2  + s1(w7);  t1 = h + S1(e) + ch(e, f, g) + 0xa81a664b + w9;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w10 += s0(w11) + w3  + s1(w8);  t1 = h + S1(e) + ch(e, f, g) + 0xc24b8b70 + w10; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w11 += s0(w12) + w4  + s1(w9);  t1 = h + S1(e) + ch(e, f, g) + 0xc76c51a3 + w11; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w12 += s0(w13) + w5  + s1(w10); t1 = h + S1(e) + ch(e, f, g) + 0xd192e819 + w12; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w13 += s0(w14) + w6  + s1(w11); t1 = h + S1(e) + ch(e, f, g) + 0xd6990624 + w13; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w14 += s0(w15) + w7  + s1(w12); t1 = h + S1(e) + ch(e, f, g) + 0xf40e3585 + w14; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w15 += s0(w0)  + w8  + s1(w13); t1 = h + S1(e) + ch(e, f, g) + 0x106aa070 + w15; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;

        // Rounds 48..63
        w0  += s0(w1)  + w9  + s1(w14); t1 = h + S1(e) + ch(e, f, g) + 0x19a4c116 + w0;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w1  += s0(w2)  + w10 + s1(w15); t1 = h + S1(e) + ch(e, f, g) + 0x1e376c08 + w1;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w2  += s0(w3)  + w11 + s1(w0);  t1 = h + S1(e) + ch(e, f, g) + 0x2748774c + w2;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w3  += s0(w4)  + w12 + s1(w1);  t1 = h + S1(e) + ch(e, f, g) + 0x34b0bcb5 + w3;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w4  += s0(w5)  + w13 + s1(w2);  t1 = h + S1(e) + ch(e, f, g) + 0x391c0cb3 + w4;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w5  += s0(w6)  + w14 + s1(w3);  t1 = h + S1(e) + ch(e, f, g) + 0x4ed8aa4a + w5;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w6  += s0(w7)  + w15 + s1(w4);  t1 = h + S1(e) + ch(e, f, g) + 0x5b9cca4f + w6;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w7  += s0(w8)  + w0  + s1(w5);  t1 = h + S1(e) + ch(e, f, g) + 0x682e6ff3 + w7;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w8  += s0(w9)  + w1  + s1(w6);  t1 = h + S1(e) + ch(e, f, g) + 0x748f82ee + w8;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w9  += s0(w10) + w2  + s1(w7);  t1 = h + S1(e) + ch(e, f, g) + 0x78a5636f + w9;  t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w10 += s0(w11) + w3  + s1(w8);  t1 = h + S1(e) + ch(e, f, g) + 0x84c87814 + w10; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w11 += s0(w12) + w4  + s1(w9);  t1 = h + S1(e) + ch(e, f, g) + 0x8cc70208 + w11; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w12 += s0(w13) + w5  + s1(w10); t1 = h + S1(e) + ch(e, f, g) + 0x90befffa + w12; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w13 += s0(w14) + w6  + s1(w11); t1 = h + S1(e) + ch(e, f, g) + 0xa4506ceb + w13; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w14 += s0(w15) + w7  + s1(w12); t1 = h + S1(e) + ch(e, f, g) + 0xbef9a3f7 + w14; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        w15 += s0(w0)  + w8  + s1(w13); t1 = h + S1(e) + ch(e, f, g) + 0xc67178f2 + w15; t2 = S0(a) + maj(a, b, c); h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;

        state.set(JAVA_INT_UNALIGNED, 0L,  h0 + a);
        state.set(JAVA_INT_UNALIGNED, 4L,  h1 + b);
        state.set(JAVA_INT_UNALIGNED, 8L,  h2 + c);
        state.set(JAVA_INT_UNALIGNED, 12L, h3 + d);
        state.set(JAVA_INT_UNALIGNED, 16L, h4 + e);
        state.set(JAVA_INT_UNALIGNED, 20L, h5 + f);
        state.set(JAVA_INT_UNALIGNED, 24L, h6 + g);
        state.set(JAVA_INT_UNALIGNED, 28L, h7 + h);
    }

    private static int S0(int x) {
        return Integer.rotateRight(x, 2) ^ Integer.rotateRight(x, 13) ^ Integer.rotateRight(x, 22);
    }

    private static int S1(int x) {
        return Integer.rotateRight(x, 6) ^ Integer.rotateRight(x, 11) ^ Integer.rotateRight(x, 25);
    }

    private static int s0(int x) {
        return Integer.rotateRight(x, 7) ^ Integer.rotateRight(x, 18) ^ (x >>> 3);
    }

    private static int s1(int x) {
        return Integer.rotateRight(x, 17) ^ Integer.rotateRight(x, 19) ^ (x >>> 10);
    }

    private static int ch(int x, int y, int z) {
        return (x & y) ^ (~x & z);
    }

    private static int maj(int x, int y, int z) {
        return (x & y) ^ (x & z) ^ (y & z);
    }

    private static void zero64B(MemorySegment seg, long off) {
        seg.set(JAVA_LONG_UNALIGNED, off,       0L);
        seg.set(JAVA_LONG_UNALIGNED, off + 8L,  0L);
        seg.set(JAVA_LONG_UNALIGNED, off + 16L, 0L);
        seg.set(JAVA_LONG_UNALIGNED, off + 24L, 0L);
        seg.set(JAVA_LONG_UNALIGNED, off + 32L, 0L);
        seg.set(JAVA_LONG_UNALIGNED, off + 40L, 0L);
        seg.set(JAVA_LONG_UNALIGNED, off + 48L, 0L);
        seg.set(JAVA_LONG_UNALIGNED, off + 56L, 0L);
    }

    private static void copyBytes(MemorySegment src, long srcOff, MemorySegment dst, long dstOff, int len) {
        int i = 0;
        for (; i <= len - 8; i += 8) {
            dst.set(JAVA_LONG_UNALIGNED, dstOff + i, src.get(JAVA_LONG_UNALIGNED, srcOff + i));
        }
        for (; i < len; i++) {
            dst.set(JAVA_BYTE, dstOff + i, src.get(JAVA_BYTE, srcOff + i));
        }
    }

    private static int decodeBase64Url(
            MemorySegment src, long srcOff, int srcLen,
            MemorySegment dst, long dstOff) {

        while (srcLen > 0 && src.get(JAVA_BYTE, srcOff + srcLen - 1) == '=') {
            srcLen--;
        }
        if (srcLen <= 0 || srcLen > MAX_B64_LEN || (srcLen & 3) == 1) {
            return -1;
        }

        int fullQuads = srcLen & ~3;
        int outPos = 0;
        for (int i = 0; i < fullQuads; i += 4) {
            int b0 = B64_DEC[src.get(JAVA_BYTE, srcOff + i)     & 0xFF];
            int b1 = B64_DEC[src.get(JAVA_BYTE, srcOff + i + 1) & 0xFF];
            int b2 = B64_DEC[src.get(JAVA_BYTE, srcOff + i + 2) & 0xFF];
            int b3 = B64_DEC[src.get(JAVA_BYTE, srcOff + i + 3) & 0xFF];
            if ((b0 | b1 | b2 | b3) < 0) return -1;

            int triple = (b0 << 18) | (b1 << 12) | (b2 << 6) | b3;
            dst.set(JAVA_BYTE, dstOff + outPos,     (byte) (triple >>> 16));
            dst.set(JAVA_BYTE, dstOff + outPos + 1, (byte) (triple >>> 8));
            dst.set(JAVA_BYTE, dstOff + outPos + 2, (byte) triple);
            outPos += 3;
        }

        int rem = srcLen - fullQuads;
        if (rem == 2) {
            int b0 = B64_DEC[src.get(JAVA_BYTE, srcOff + fullQuads)     & 0xFF];
            int b1 = B64_DEC[src.get(JAVA_BYTE, srcOff + fullQuads + 1) & 0xFF];
            if ((b0 | b1) < 0) return -1;
            dst.set(JAVA_BYTE, dstOff + outPos++, (byte) ((b0 << 2) | (b1 >>> 4)));
        } else if (rem == 3) {
            int b0 = B64_DEC[src.get(JAVA_BYTE, srcOff + fullQuads)     & 0xFF];
            int b1 = B64_DEC[src.get(JAVA_BYTE, srcOff + fullQuads + 1) & 0xFF];
            int b2 = B64_DEC[src.get(JAVA_BYTE, srcOff + fullQuads + 2) & 0xFF];
            if ((b0 | b1 | b2) < 0) return -1;
            int v = (b0 << 12) | (b1 << 6) | b2;
            dst.set(JAVA_BYTE, dstOff + outPos++, (byte) (v >>> 10));
            dst.set(JAVA_BYTE, dstOff + outPos++, (byte) (v >>> 2));
        }

        return outPos;
    }

    private static boolean verifyHeaderSlow(
            MemorySegment reqSeg, long hdrOff, int hdrB64Len, MemorySegment scratch) {

        int len = decodeBase64Url(reqSeg, hdrOff, hdrB64Len, scratch, DECODED_OFF);
        if (len < 13) return false;

        int start = 0;
        while (start < len && isWs(scratch.get(JAVA_BYTE, DECODED_OFF + start))) start++;
        int end = len - 1;
        while (end > start && isWs(scratch.get(JAVA_BYTE, DECODED_OFF + end))) end--;
        if (scratch.get(JAVA_BYTE, DECODED_OFF + start) != '{' ||
            scratch.get(JAVA_BYTE, DECODED_OFF + end)   != '}') {
            return false;
        }

        boolean algHs256 = false;
        for (int i = start + 1; i <= end - 5; i++) {
            if (scratch.get(JAVA_BYTE, DECODED_OFF + i) == '"') {
                if (i + 5 < end &&
                    scratch.get(JAVA_BYTE, DECODED_OFF + i + 1) == 'a' &&
                    scratch.get(JAVA_BYTE, DECODED_OFF + i + 2) == 'l' &&
                    scratch.get(JAVA_BYTE, DECODED_OFF + i + 3) == 'g' &&
                    scratch.get(JAVA_BYTE, DECODED_OFF + i + 4) == '"') {
                    int p = i + 5;
                    while (p < end && isWs(scratch.get(JAVA_BYTE, DECODED_OFF + p))) p++;
                    if (p >= end || scratch.get(JAVA_BYTE, DECODED_OFF + p) != ':') return false;
                    p++;
                    while (p < end && isWs(scratch.get(JAVA_BYTE, DECODED_OFF + p))) p++;
                    if (p + 7 <= end &&
                        scratch.get(JAVA_BYTE, DECODED_OFF + p)     == '"' &&
                        scratch.get(JAVA_BYTE, DECODED_OFF + p + 1) == 'H' &&
                        scratch.get(JAVA_BYTE, DECODED_OFF + p + 2) == 'S' &&
                        scratch.get(JAVA_BYTE, DECODED_OFF + p + 3) == '2' &&
                        scratch.get(JAVA_BYTE, DECODED_OFF + p + 4) == '5' &&
                        scratch.get(JAVA_BYTE, DECODED_OFF + p + 5) == '6' &&
                        scratch.get(JAVA_BYTE, DECODED_OFF + p + 6) == '"') {
                        algHs256 = true;
                        i = p + 6;
                    } else {
                        return false;
                    }
                }
            }
        }
        return algHs256;
    }

    /**
     * Extracts and validates "sub" (> 0), "exp" (> nowEpochSec), and "username" (non-empty string)
     * from the decoded JSON payload at seg[off .. off + len).
     * Returns userId (> 0) on success, -1L on invalid/expired token, or -2L on invalid token payload.
     */
    private static long parseAndValidateClaims(
            MemorySegment seg, long off, int len, long nowEpochSec) {

        int i = 0;
        while (i < len && isWs(seg.get(JAVA_BYTE, off + i))) i++;
        int end = len - 1;
        while (end > i && isWs(seg.get(JAVA_BYTE, off + end))) end--;
        if (end - i < 15 ||
            seg.get(JAVA_BYTE, off + i)   != '{' ||
            seg.get(JAVA_BYTE, off + end) != '}') {
            return -1L;
        }
        i++;

        long sub = -1L;
        long exp = -1L;
        int userOff = 0;
        int userLen = 0;
        boolean badPayload = false;

        while (i < end) {
            byte b = seg.get(JAVA_BYTE, off + i);
            if (b != '"') {
                i++;
                continue;
            }
            int keyStart = ++i;
            while (i < end && seg.get(JAVA_BYTE, off + i) != '"') {
                if (seg.get(JAVA_BYTE, off + i) == '\\') i++;
                i++;
            }
            if (i >= end) return -1L;
            int keyLen = i - keyStart;
            i++; // skip closing '"'

            while (i < end && isWs(seg.get(JAVA_BYTE, off + i))) i++;
            if (i >= end || seg.get(JAVA_BYTE, off + i) != ':') return -1L;
            i++;
            while (i < end && isWs(seg.get(JAVA_BYTE, off + i))) i++;
            if (i >= end) return -1L;

            if (keyLen == 3 &&
                seg.get(JAVA_BYTE, off + keyStart)     == 's' &&
                seg.get(JAVA_BYTE, off + keyStart + 1) == 'u' &&
                seg.get(JAVA_BYTE, off + keyStart + 2) == 'b') {

                if (seg.get(JAVA_BYTE, off + i) == '"') {
                    i++;
                    long val = 0L;
                    int digits = 0;
                    while (i < end) {
                        int d = seg.get(JAVA_BYTE, off + i) - '0';
                        if (d < 0 || d > 9) break;
                        val = val * 10L + d;
                        digits++;
                        i++;
                    }
                    if (digits == 0 || digits > 18 || val <= 0L || i >= end || seg.get(JAVA_BYTE, off + i) != '"') {
                        badPayload = true;
                        while (i < end && seg.get(JAVA_BYTE, off + i) != '"') {
                            if (seg.get(JAVA_BYTE, off + i) == '\\') i++;
                            i++;
                        }
                        if (i >= end) return -1L;
                        i++;
                    } else {
                        i++;
                        sub = val;
                    }
                } else {
                    long val = 0L;
                    int digits = 0;
                    while (i < end) {
                        int d = seg.get(JAVA_BYTE, off + i) - '0';
                        if (d < 0 || d > 9) break;
                        val = val * 10L + d;
                        digits++;
                        i++;
                    }
                    byte next = seg.get(JAVA_BYTE, off + i);
                    if (digits == 0 || digits > 18 || val <= 0L || (next != ',' && next != '}' && !isWs(next))) {
                        badPayload = true;
                        while (i < end) {
                            byte vc = seg.get(JAVA_BYTE, off + i);
                            if (vc == ',' || vc == '}' || isWs(vc)) break;
                            i++;
                        }
                    } else {
                        sub = val;
                    }
                }

            } else if (keyLen == 3 &&
                       seg.get(JAVA_BYTE, off + keyStart)     == 'e' &&
                       seg.get(JAVA_BYTE, off + keyStart + 1) == 'x' &&
                       seg.get(JAVA_BYTE, off + keyStart + 2) == 'p') {

                long val = 0L;
                int digits = 0;
                while (i < end) {
                    int d = seg.get(JAVA_BYTE, off + i) - '0';
                    if (d < 0 || d > 9) break;
                    val = val * 10L + d;
                    digits++;
                    i++;
                }
                if (digits == 0 || digits > 18 || val <= 0L) return -1L;
                byte next = seg.get(JAVA_BYTE, off + i);
                if (next != ',' && next != '}' && !isWs(next)) return -1L;
                exp = val;

            } else if (keyLen == 8 &&
                       seg.get(JAVA_BYTE, off + keyStart)     == 'u' &&
                       seg.get(JAVA_BYTE, off + keyStart + 1) == 's' &&
                       seg.get(JAVA_BYTE, off + keyStart + 2) == 'e' &&
                       seg.get(JAVA_BYTE, off + keyStart + 3) == 'r' &&
                       seg.get(JAVA_BYTE, off + keyStart + 4) == 'n' &&
                       seg.get(JAVA_BYTE, off + keyStart + 5) == 'a' &&
                       seg.get(JAVA_BYTE, off + keyStart + 6) == 'm' &&
                       seg.get(JAVA_BYTE, off + keyStart + 7) == 'e') {

                if (seg.get(JAVA_BYTE, off + i) != '"') {
                    badPayload = true;
                    while (i < end) {
                        byte vc = seg.get(JAVA_BYTE, off + i);
                        if (vc == ',' || vc == '}' || isWs(vc)) break;
                        i++;
                    }
                } else {
                    int uStart = ++i;
                    while (i < end && seg.get(JAVA_BYTE, off + i) != '"') {
                        if (seg.get(JAVA_BYTE, off + i) == '\\') i++;
                        i++;
                    }
                    if (i >= end) return -1L;
                    userOff = (int) (off + uStart);
                    userLen = i - uStart;
                    if (userLen <= 0) {
                        badPayload = true;
                    }
                    i++; // skip closing '"'
                }

            } else {
                // Skip other JSON value (e.g., "iat")
                byte c = seg.get(JAVA_BYTE, off + i);
                if (c == '"') {
                    i++;
                    while (i < end) {
                        byte sc = seg.get(JAVA_BYTE, off + i++);
                        if (sc == '"') break;
                        if (sc == '\\') i++;
                    }
                } else {
                    while (i < end) {
                        byte vc = seg.get(JAVA_BYTE, off + i);
                        if (vc == ',' || vc == '}' || isWs(vc)) break;
                        i++;
                    }
                }
            }
        }

        if (exp <= nowEpochSec) {
            return -1L;
        }
        if (badPayload || sub <= 0L || userLen <= 0) {
            return -2L;
        }
        seg.set(JAVA_INT_UNALIGNED, USERNAME_OFF_POS, userOff);
        seg.set(JAVA_INT_UNALIGNED, USERNAME_LEN_POS, userLen);
        return sub;
    }

    private static boolean isWs(byte b) {
        return b == ' ' || b == '\t' || b == '\r' || b == '\n';
    }

    private static void selfTest(String secret, Arena arena) throws Exception {
        String hdr = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
        String pay = java.util.Base64.getUrlEncoder().withoutPadding()
                .encodeToString("{\"sub\":\"424242\",\"username\":\"user_424242\",\"iat\":1700000000,\"exp\":4102444800}"
                        .getBytes(StandardCharsets.UTF_8));
        String signed = hdr + "." + pay;

        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(secret.getBytes(StandardCharsets.UTF_8), "HmacSHA256"));
        String sig = java.util.Base64.getUrlEncoder().withoutPadding()
                .encodeToString(mac.doFinal(signed.getBytes(StandardCharsets.UTF_8)));

        byte[] fullHeader = ("Bearer " + signed + "." + sig).getBytes(StandardCharsets.US_ASCII);
        MemorySegment req = arena.allocate(fullHeader.length);
        MemorySegment.copy(fullHeader, 0, req, JAVA_BYTE, 0L, fullHeader.length);
        MemorySegment scratch = arena.allocate(SCRATCH_BYTES, 8L);

        long userId = verifyAuthHeader(req, 0L, fullHeader.length, 1700000000L, scratch);
        if (userId != 424242L || usernameLength(scratch) != 11) {
            throw new AssertionError("Jwt self-test failed: userId=" + userId);
        }
    }
}
