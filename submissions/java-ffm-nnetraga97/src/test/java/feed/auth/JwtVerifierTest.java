package feed.auth;

import static feed.auth.JwtVerifier.Result.BAD_PAYLOAD;
import static feed.auth.JwtVerifier.Result.INVALID;
import static feed.auth.JwtVerifier.Result.MISSING;
import static feed.auth.JwtVerifier.Result.VALID;
import static feed.testing.Tokens.b64;
import static feed.testing.Tokens.sign;
import static org.junit.jupiter.api.Assertions.assertEquals;

import java.nio.charset.StandardCharsets;

import org.junit.jupiter.api.Test;

class JwtVerifierTest {

    private static final String SECRET = "million-users-challenge";
    private static final long NOW = 1_800_000_000L;
    private static final String HS256 = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";

    private final JwtVerifier jwt = new JwtVerifier(SECRET.getBytes(StandardCharsets.UTF_8));

    static String claims(String sub, String username, long exp) {
        return "{\"sub\":" + sub + ",\"username\":" + username + ",\"iat\":" + (NOW - 10) + ",\"exp\":" + exp + "}";
    }

    private JwtVerifier.Result verify(String headerValue) {
        byte[] b = ("::" + headerValue).getBytes(StandardCharsets.UTF_8);
        return jwt.verify(b, 2, b.length - 2, NOW);
    }

    private String username() {
        return new String(jwt.username(), 0, jwt.usernameLength(), StandardCharsets.UTF_8);
    }

    @Test
    void acceptsAValidToken() throws Exception {
        assertEquals(VALID, verify("Bearer " + sign(HS256, claims("\"1\"", "\"golden_ember_1\"", NOW + 3600))));
        assertEquals(1, jwt.userId());
        assertEquals("golden_ember_1", username());
    }

    @Test
    void acceptsTheSeedTokenShape() throws Exception {
        String payload = "{\"sub\":\"20000\",\"username\":\"quiet_thistle_7672\",\"iat\":1767225600,\"exp\":2082585600}";
        assertEquals(VALID, verify("Bearer " + sign(HS256, payload)));
        assertEquals(20000, jwt.userId());
        assertEquals("quiet_thistle_7672", username());
    }

    @Test
    void decodesEscapedUsernames() throws Exception {
        assertEquals(VALID, verify("Bearer " + sign(HS256, claims("\"7\"", "\"a\\\"b caf\\u00e9\"", NOW + 60))));
        assertEquals("a\"b café", username());
    }

    @Test
    void acceptsAPaddedSignature() throws Exception {
        String token = sign(HS256, claims("\"1\"", "\"u\"", NOW + 60));
        assertEquals(VALID, verify("Bearer " + token + "="));
        assertEquals(INVALID, verify("Bearer " + token + "=x"));
    }

    @Test
    void missingOrNonBearerHeaderIsMissing() {
        byte[] none = new byte[0];
        assertEquals(MISSING, jwt.verify(none, 0, -1, NOW));
        assertEquals(MISSING, verify("Basic abc"));
        assertEquals(MISSING, verify("bearer abc"));
        assertEquals(MISSING, verify("Bearer"));
    }

    @Test
    void garbageIsInvalid() {
        assertEquals(INVALID, verify("Bearer abc"));
        assertEquals(INVALID, verify("Bearer "));
        assertEquals(INVALID, verify("Bearer a.b.c"));
        assertEquals(INVALID, verify("Bearer a.b.c.d"));
        assertEquals(INVALID, verify("Bearer !!.??.**"));
    }

    @Test
    void aBadSignatureIsInvalid() throws Exception {
        String token = sign(HS256, claims("\"1\"", "\"u\"", NOW + 60));
        String tampered = token.substring(0, token.lastIndexOf('.'))
                + ".c2lnbmF0dXJlLWlzLW5vdC12YWxpZC1hdC1hbGwtYXQtYWxsLTEyMzQ";
        assertEquals(INVALID, verify("Bearer " + tampered));
    }

    @Test
    void aSignatureFromAnotherPayloadIsInvalid() throws Exception {
        String a = sign(HS256, claims("\"1\"", "\"u\"", NOW + 60));
        String b = sign(HS256, claims("\"2\"", "\"u\"", NOW + 60));
        String mixed = a.substring(0, a.lastIndexOf('.')) + b.substring(b.lastIndexOf('.'));
        assertEquals(INVALID, verify("Bearer " + mixed));
    }

    @Test
    void otherAlgorithmsAreInvalid() throws Exception {
        String payload = claims("\"1\"", "\"u\"", NOW + 60);
        assertEquals(INVALID, verify("Bearer " + b64("{\"alg\":\"none\",\"typ\":\"JWT\"}") + "." + b64(payload) + "."));
        assertEquals(INVALID, verify("Bearer " + sign("{\"alg\":\"HS384\"}", payload)));
        assertEquals(INVALID, verify("Bearer " + sign("{\"typ\":\"JWT\"}", payload)));
        assertEquals(INVALID, verify("Bearer " + sign("[\"HS256\"]", payload)));
    }

    @Test
    void expiredOrExpLessTokensAreInvalid() throws Exception {
        assertEquals(INVALID, verify("Bearer " + sign(HS256, claims("\"1\"", "\"u\"", NOW - 3600))));
        assertEquals(INVALID, verify("Bearer " + sign(HS256, claims("\"1\"", "\"u\"", NOW))));
        assertEquals(INVALID, verify("Bearer " + sign(HS256, "{\"sub\":\"1\",\"username\":\"u\"}")));
        assertEquals(INVALID, verify("Bearer " + sign(HS256, "{\"sub\":\"1\",\"username\":\"u\",\"exp\":\"9999999999\"}")));
    }

    @Test
    void aFutureNotBeforeIsInvalid() throws Exception {
        String payload = "{\"sub\":\"1\",\"username\":\"u\",\"exp\":" + (NOW + 60) + ",\"nbf\":" + (NOW + 30) + "}";
        assertEquals(INVALID, verify("Bearer " + sign(HS256, payload)));
        String ok = "{\"sub\":\"1\",\"username\":\"u\",\"exp\":" + (NOW + 60) + ",\"nbf\":" + (NOW - 30) + "}";
        assertEquals(VALID, verify("Bearer " + sign(HS256, ok)));
    }

    @Test
    void malformedPayloadJsonIsInvalid() throws Exception {
        assertEquals(INVALID, verify("Bearer " + sign(HS256, "{not json")));
        assertEquals(INVALID, verify("Bearer " + sign(HS256, "[1,2]")));
    }

    @Test
    void aBadSubjectIsABadPayload() throws Exception {
        assertEquals(BAD_PAYLOAD, verify("Bearer " + sign(HS256, claims("\"abc\"", "\"u\"", NOW + 60))));
        assertEquals(BAD_PAYLOAD, verify("Bearer " + sign(HS256, claims("7", "\"u\"", NOW + 60))));
        assertEquals(BAD_PAYLOAD, verify("Bearer " + sign(HS256, claims("\"0\"", "\"u\"", NOW + 60))));
        assertEquals(BAD_PAYLOAD, verify("Bearer " + sign(HS256, claims("\"-3\"", "\"u\"", NOW + 60))));
        assertEquals(BAD_PAYLOAD, verify("Bearer " + sign(HS256, claims("\"\"", "\"u\"", NOW + 60))));
        assertEquals(BAD_PAYLOAD, verify("Bearer " + sign(HS256, claims("\"99999999999999999999\"", "\"u\"", NOW + 60))));
    }

    @Test
    void aMissingOrNonStringUsernameIsABadPayload() throws Exception {
        assertEquals(BAD_PAYLOAD,
                verify("Bearer " + sign(HS256, "{\"sub\":\"1\",\"exp\":" + (NOW + 60) + "}")));
        assertEquals(BAD_PAYLOAD, verify("Bearer " + sign(HS256, claims("\"1\"", "42", NOW + 60))));
        assertEquals(BAD_PAYLOAD, verify("Bearer " + sign(HS256, claims("\"1\"", "null", NOW + 60))));
    }
}
