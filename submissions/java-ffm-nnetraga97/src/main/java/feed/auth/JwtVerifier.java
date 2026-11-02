package feed.auth;

import java.security.GeneralSecurityException;

import javax.crypto.Mac;
import javax.crypto.ShortBufferException;
import javax.crypto.spec.SecretKeySpec;

import feed.http.Bytes;
import feed.json.JsonReader;

/**
 * Verifies {@code Authorization: Bearer <jwt>} on every request, with nothing remembered between requests. HS256
 * only: alg must be "HS256", the signature must match, exp must be in the future and nbf (if present) not; then sub
 * must be a string holding a positive integer and username a string. Allocation-free; one per worker.
 */
public final class JwtVerifier {

    public enum Result {
        VALID,
        /** No {@code Bearer} token. */
        MISSING,
        /** Malformed, wrong algorithm, bad signature, expired or not yet valid. */
        INVALID,
        /** Signed and current, but sub or username is missing or malformed. */
        BAD_PAYLOAD
    }

    private static final byte[] BEARER = {'B', 'e', 'a', 'r', 'e', 'r', ' '};
    private static final byte[] DECODE = new byte[128];

    static {
        java.util.Arrays.fill(DECODE, (byte) -1);
        String alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        for (int i = 0; i < alphabet.length(); i++) {
            DECODE[alphabet.charAt(i)] = (byte) i;
        }
    }

    private final Mac mac;
    private final byte[] expected = new byte[32];
    private final byte[] signature = new byte[64];
    private final byte[] subject = new byte[128];
    private final JsonReader header = new JsonReader("alg");
    private final JsonReader claims = new JsonReader("sub", "username", "exp", "nbf");
    private byte[] decoded = new byte[1024];
    private byte[] username = new byte[64];
    private int usernameLength;
    private long userId;

    public JwtVerifier(byte[] secret) {
        try {
            mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(secret, "HmacSHA256"));
        } catch (GeneralSecurityException e) {
            throw new IllegalStateException(e);
        }
    }

    /**
     * Checks the Authorization header value in {@code b[off, off+len)}; {@code len < 0} means there was no header.
     * When {@link Result#VALID}, {@link #userId()} and {@link #username()} describe the token's user.
     */
    public Result verify(byte[] b, int off, int len, long nowSeconds) {
        if (len < BEARER.length || !java.util.Arrays.equals(b, off, off + BEARER.length, BEARER, 0, BEARER.length)) {
            return Result.MISSING;
        }
        int start = off + BEARER.length;
        int end = off + len;
        int dot1 = Bytes.indexOf(b, start, end, (byte) '.');
        int dot2 = dot1 < 0 ? -1 : Bytes.indexOf(b, dot1 + 1, end, (byte) '.');
        if (dot2 < 0 || Bytes.indexOf(b, dot2 + 1, end, (byte) '.') >= 0) {
            return Result.INVALID;
        }

        // Header: alg must be HS256.
        int n = base64(b, start, dot1);
        if (n < 0 || header.read(decoded, 0, n) != JsonReader.OBJECT || !header.stringEquals(0, "HS256")) {
            return Result.INVALID;
        }

        // Signature over "header.payload", exactly as sent.
        int sigLength = base64Into(b, dot2 + 1, end, signature);
        if (sigLength != 32) {
            return Result.INVALID;
        }
        try {
            mac.update(b, start, dot2 - start);
            mac.doFinal(expected, 0);
        } catch (ShortBufferException e) {
            throw new IllegalStateException(e);
        }
        int diff = 0;
        for (int i = 0; i < 32; i++) {
            diff |= expected[i] ^ signature[i];
        }
        if (diff != 0) {
            return Result.INVALID;
        }

        // Claims: exp (and nbf) first, then the payload checks.
        n = base64(b, dot1 + 1, dot2);
        if (n < 0 || claims.read(decoded, 0, n) != JsonReader.OBJECT) {
            return Result.INVALID;
        }
        if (claims.type(2) != JsonReader.NUMBER || !(claims.number(2) > nowSeconds)) {
            return Result.INVALID;
        }
        if (claims.type(3) != JsonReader.MISSING
                && (claims.type(3) != JsonReader.NUMBER || claims.number(3) > nowSeconds)) {
            return Result.INVALID;
        }
        if (claims.type(0) != JsonReader.STRING || claims.type(1) != JsonReader.STRING) {
            return Result.BAD_PAYLOAD;
        }
        userId = positiveInt(claims);
        if (userId <= 0) {
            return Result.BAD_PAYLOAD;
        }
        if (username.length < claims.maxDecodedLength(1)) {
            username = new byte[claims.maxDecodedLength(1)];
        }
        usernameLength = claims.decodeString(1, username, 0);
        return Result.VALID;
    }

    public long userId() {
        return userId;
    }

    /** The username's UTF-8 bytes, {@link #usernameLength()} long. */
    public byte[] username() {
        return username;
    }

    public int usernameLength() {
        return usernameLength;
    }

    /** sub as a positive integer of at most 18 digits, or 0. */
    private long positiveInt(JsonReader r) {
        if (r.maxDecodedLength(0) > subject.length) {
            return 0;
        }
        return Bytes.positiveId(subject, 0, r.decodeString(0, subject, 0));
    }

    /** Decodes base64url {@code b[from, to)} into {@link #decoded}; returns the length or -1. */
    private int base64(byte[] b, int from, int to) {
        int max = (to - from) * 3 / 4 + 3;
        if (decoded.length < max) {
            decoded = new byte[Math.max(max, decoded.length * 2)];
        }
        return base64Into(b, from, to, decoded);
    }

    /** Base64url, padding optional ('=' only at the end). Returns the decoded length, or -1. */
    private static int base64Into(byte[] b, int from, int to, byte[] out) {
        for (int pad = 0; pad < 2 && to > from && b[to - 1] == '='; pad++) {
            to--;
        }
        int len = to - from;
        if (len % 4 == 1 || (len * 3) / 4 > out.length) {
            return -1;
        }
        int o = 0;
        int acc = 0;
        int bits = 0;
        for (int i = from; i < to; i++) {
            int c = b[i];
            int v = c >= 0 && c < 128 ? DECODE[c] : -1;
            if (v < 0) {
                return -1;
            }
            acc = (acc << 6) | v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out[o++] = (byte) (acc >> bits);
            }
        }
        return o;
    }
}
