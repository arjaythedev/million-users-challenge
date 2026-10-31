package feed.json;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

class JsonReaderTest {

    private static int read(JsonReader r, String json) {
        byte[] b = ("xx" + json + "yy").getBytes(StandardCharsets.UTF_8);
        return r.read(b, 2, b.length - 2);
    }

    private static String decoded(JsonReader r, int key) {
        byte[] out = new byte[r.maxDecodedLength(key) + 3];
        int end = r.decodeString(key, out, 3);
        return new String(out, 3, end - 3, StandardCharsets.UTF_8);
    }

    @ParameterizedTest
    @ValueSource(strings = {
        "{}", " { } ", "{\"a\":1}", "{\"a\":-0.5e+10,\"b\":[1,{\"c\":null},true,false],\"d\":{\"e\":\"f\"}}",
        "{\"s\":\"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u00e9\"}", "{\"a\":0}", "{\"a\":[]}", "{\"a\" : \"b\" , \"c\" : 2}",
    })
    void acceptsValidObjects(String json) {
        assertEquals(JsonReader.OBJECT, read(new JsonReader(), json));
    }

    @ParameterizedTest
    @ValueSource(strings = {"[]", "\"x\"", "1", "null", "true", "[{\"body\":\"x\"}]"})
    void recognisesValidNonObjects(String json) {
        assertEquals(JsonReader.NOT_OBJECT, read(new JsonReader("body"), json));
    }

    @ParameterizedTest
    @ValueSource(strings = {
        "", " ", "{bad json", "{", "}", "{\"a\"}", "{\"a\":}", "{\"a\":1,}", "{,}", "{\"a\":01}", "{\"a\":1.}",
        "{\"a\":.5}", "{\"a\":+1}", "{\"a\":tru}", "{\"a\":nul}", "{'a':1}", "{a:1}", "{\"a\":\"\\x\"}",
        "{\"a\":\"\\u12g4\"}", "{\"a\":\"line\nbreak\"}", "{\"a\":1} {}", "{\"a\":[1,]}", "{\"a\":\"unterminated}",
        "{\"a\":1e}", "{\"a\":-}",
    })
    void rejectsInvalidJson(String json) {
        assertEquals(JsonReader.INVALID, read(new JsonReader("a"), json));
    }

    @Test
    void rejectsVeryDeepNesting() {
        assertEquals(JsonReader.INVALID, read(new JsonReader(), "{\"a\":" + "[".repeat(1000) + "]".repeat(1000) + "}"));
    }

    @Test
    void capturesWantedMembersOnly() {
        JsonReader r = new JsonReader("body", "n");
        assertEquals(JsonReader.OBJECT, read(r, "{\"x\":{\"body\":\"inner\"},\"body\":\"outer\",\"n\":42}"));
        assertEquals(JsonReader.STRING, r.type(0));
        assertEquals("outer", decoded(r, 0));
        assertEquals(JsonReader.NUMBER, r.type(1));
        assertEquals(42.0, r.number(1));
    }

    @Test
    void reportsMissingAndNonStringMembers() {
        JsonReader r = new JsonReader("body", "other");
        read(r, "{\"body\":123}");
        assertEquals(JsonReader.NUMBER, r.type(0));
        assertEquals(JsonReader.MISSING, r.type(1));
        read(r, "{\"body\":null}");
        assertEquals(JsonReader.OTHER, r.type(0));
    }

    @Test
    void forgetsMembersFromThePreviousDocument() {
        JsonReader r = new JsonReader("body");
        read(r, "{\"body\":\"x\"}");
        read(r, "{}");
        assertEquals(JsonReader.MISSING, r.type(0));
    }

    @Test
    void lastDuplicateWins() {
        JsonReader r = new JsonReader("body");
        read(r, "{\"body\":\"first\",\"body\":\"second\"}");
        assertEquals("second", decoded(r, 0));
    }

    @Test
    void decodesEscapesAndSurrogatePairs() {
        JsonReader r = new JsonReader("s");
        read(r, "{\"s\":\"q\\\"b\\\\s\\/n\\nt\\tu\\u00e9 \\ud83d\\ude00 café ✓\"}");
        assertEquals("q\"b\\s/n\nt\tué 😀 café ✓", decoded(r, 0));
    }

    @Test
    void matchesEscapedKeys() {
        JsonReader r = new JsonReader("body");
        read(r, "{\"b\\u006fdy\":\"x\"}");
        assertEquals("x", decoded(r, 0));
    }

    @Test
    void comparesStringsAfterDecoding() {
        JsonReader r = new JsonReader("alg");
        read(r, "{\"alg\":\"HS\\u0032\\u00356\"}");
        assertTrue(r.stringEquals(0, "HS256"));
        assertFalse(r.stringEquals(0, "HS25"));
        read(r, "{\"alg\":\"none\"}");
        assertFalse(r.stringEquals(0, "HS256"));
    }

    @Test
    void parsesNumbers() {
        JsonReader r = new JsonReader("n");
        read(r, "{\"n\":1767225600}");
        assertEquals(1767225600.0, r.number(0));
        read(r, "{\"n\":-12.75}");
        assertEquals(-12.75, r.number(0));
        read(r, "{\"n\":1.5e3}");
        assertEquals(1500.0, r.number(0));
    }
}
