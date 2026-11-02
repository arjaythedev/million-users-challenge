package feed.testing;

import java.nio.charset.StandardCharsets;
import java.util.Base64;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/** HS256 tokens signed with the challenge's JWT_SECRET. */
public final class Tokens {

    public static final String SECRET = "million-users-challenge";
    public static final String HS256 = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";

    private Tokens() {
    }

    public static String b64(String s) {
        return Base64.getUrlEncoder().withoutPadding().encodeToString(s.getBytes(StandardCharsets.UTF_8));
    }

    /** header.payload.signature, the payload signed as given. */
    public static String sign(String header, String payload) {
        try {
            String signingInput = b64(header) + "." + b64(payload);
            Mac mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(SECRET.getBytes(StandardCharsets.UTF_8), "HmacSHA256"));
            byte[] sig = mac.doFinal(signingInput.getBytes(StandardCharsets.US_ASCII));
            return signingInput + "." + Base64.getUrlEncoder().withoutPadding().encodeToString(sig);
        } catch (java.security.GeneralSecurityException e) {
            throw new IllegalStateException(e);
        }
    }

    /** A valid token for {@code sub}/{@code username}, expiring at {@code exp} (unix seconds). */
    public static String token(String sub, String username, long exp) {
        return sign(HS256, "{\"sub\":\"" + sub + "\",\"username\":\"" + username + "\",\"iat\":" + (exp - 3600)
                + ",\"exp\":" + exp + "}");
    }
}
