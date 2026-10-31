package challenge;

import java.security.GeneralSecurityException;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Base64;
import java.util.Map;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/** `Authorization: Bearer <jwt>`: HS256 signature and `exp` are checked on every request, nothing is remembered. */
final class Jwt {
    static final class Rejected extends RuntimeException {
        Rejected(String message) {
            super(message, null, false, false);
        }
    }

    static final Rejected MISSING = new Rejected("missing bearer token");
    static final Rejected INVALID = new Rejected("invalid or expired token");
    static final Rejected PAYLOAD = new Rejected("invalid token payload");

    record User(long id, String name) {}

    private static final Base64.Decoder B64 = Base64.getUrlDecoder();
    private final Mac mac;

    /** One instance per event loop: a Mac is not thread-safe and a loop is a single thread. */
    Jwt(byte[] secret) {
        try {
            mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(secret, "HmacSHA256"));
        } catch (GeneralSecurityException e) {
            throw new IllegalStateException(e);
        }
    }

    /** `off < 0` means there was no Authorization header. `nowSeconds` is the unix time in seconds. */
    User verify(byte[] b, int off, int len, double nowSeconds) {
        if (off < 0 || len < 7 || b[off] != 'B' || b[off + 1] != 'e' || b[off + 2] != 'a' || b[off + 3] != 'r'
                || b[off + 4] != 'e' || b[off + 5] != 'r' || b[off + 6] != ' ') {
            throw MISSING;
        }
        int t = off + 7, e = off + len;
        int d1 = -1, d2 = -1;
        for (int i = t; i < e; i++) {
            if (b[i] == '.') {
                if (d1 < 0) d1 = i;
                else if (d2 < 0) d2 = i;
                else throw INVALID;
            }
        }
        if (d2 < 0) throw INVALID;
        try {
            byte[] hd = B64.decode(Arrays.copyOfRange(b, t, d1));
            Object header = Jp.parse(hd, 0, hd.length);
            if (!(header instanceof Map<?, ?> h) || !"HS256".equals(h.get("alg"))) throw INVALID;
            byte[] sig = B64.decode(Arrays.copyOfRange(b, d2 + 1, e));
            mac.update(b, t, d2 - t);
            if (!MessageDigest.isEqual(mac.doFinal(), sig)) throw INVALID;

            byte[] payload = B64.decode(Arrays.copyOfRange(b, d1 + 1, d2));
            if (!(Jp.parse(payload, 0, payload.length) instanceof Map<?, ?> c)) throw INVALID;
            if (!(c.get("exp") instanceof Double exp) || !Double.isFinite(exp) || exp <= nowSeconds) throw INVALID;
            Object nbf = c.get("nbf");
            if (nbf != null && (!(nbf instanceof Double n) || !Double.isFinite(n) || n > nowSeconds)) throw INVALID;
            long id = c.get("sub") instanceof String sub ? positiveInt(sub) : -1;
            if (id < 0 || !(c.get("username") instanceof String name)) throw PAYLOAD;
            return new User(id, name);
        } catch (IllegalArgumentException | Jp.Bad ex) {
            throw INVALID;
        }
    }

    /** A positive integer in ASCII digits of at most 18 digits; -1 for anything else (`0`, `-1`, `1.5`, `abc`, empty). */
    static long positiveInt(String s) {
        int n = s.length();
        if (n == 0 || n > 18) return -1;
        long v = 0;
        for (int i = 0; i < n; i++) {
            char ch = s.charAt(i);
            if (ch < '0' || ch > '9') return -1;
            v = v * 10 + (ch - '0');
        }
        return v > 0 ? v : -1;
    }
}
