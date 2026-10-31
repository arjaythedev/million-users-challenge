package challenge;

import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Base64;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

final class JwtVerifier {
    record User(long id, String username) { }
    private static final Base64.Decoder DECODER = Base64.getUrlDecoder();
    private final Mac mac;
    private final byte[] expected = new byte[32], actual = new byte[32];
    private final byte[] signature43 = new byte[43], signature44 = new byte[44];
    private byte[] signed = new byte[256];

    JwtVerifier(String secret) throws GeneralSecurityException {
        if (secret.isEmpty()) throw new IllegalArgumentException("JWT_SECRET must not be empty");
        mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(secret.getBytes(StandardCharsets.UTF_8), "HmacSHA256"));
    }

    private static void ascii(String value, int start, int end, byte[] output) {
        for (int i = start; i < end; i++) {
            char c = value.charAt(i);
            if (c > 127) throw new IllegalArgumentException();
            output[i - start] = (byte) c;
        }
    }

    User verify(String authorization) {
        if (authorization == null || !authorization.startsWith("Bearer "))
            throw new ApiException(401, "missing bearer token");
        Json.Fields payload;
        try {
            int first = authorization.indexOf('.', 7);
            int second = first < 0 ? -1 : authorization.indexOf('.', first + 1);
            if (second < 0 || authorization.indexOf('.', second + 1) >= 0) throw new IllegalArgumentException();
            var header = Json.read(DECODER.decode(authorization.substring(7, first)));
            if (!"HS256".equals(header.alg)) throw new IllegalArgumentException();
            int signedLength = second - 7;
            if (signedLength > signed.length) signed = Arrays.copyOf(signed, Math.max(signedLength, signed.length * 2));
            ascii(authorization, 7, second, signed);
            mac.update(signed, 0, signedLength);
            mac.doFinal(expected, 0);
            int signatureLength = authorization.length() - second - 1;
            byte[] encoded = signatureLength == 43 ? signature43 : signatureLength == 44 ? signature44 : null;
            if (encoded == null) throw new IllegalArgumentException();
            ascii(authorization, second + 1, authorization.length(), encoded);
            if (DECODER.decode(encoded, actual) != 32 || !MessageDigest.isEqual(expected, actual)) throw new IllegalArgumentException();
            payload = Json.read(DECODER.decode(authorization.substring(first + 1, second)));
            if (payload.exp == null || payload.exp <= System.currentTimeMillis() / 1000.0) throw new IllegalArgumentException();
        } catch (Exception e) {
            throw new ApiException(401, "invalid or expired token");
        }
        try {
            if (payload.sub == null || payload.username == null) throw new IllegalArgumentException();
            for (int i = 0; i < payload.sub.length(); i++)
                if (payload.sub.charAt(i) < '0' || payload.sub.charAt(i) > '9') throw new IllegalArgumentException();
            long id = Long.parseLong(payload.sub);
            if (id <= 0) throw new IllegalArgumentException();
            return new User(id, payload.username);
        } catch (Exception e) {
            throw new ApiException(401, "invalid token payload");
        }
    }
}
