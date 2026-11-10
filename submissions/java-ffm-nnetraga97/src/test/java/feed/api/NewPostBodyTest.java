package feed.api;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.nio.charset.StandardCharsets;

import org.junit.jupiter.api.Test;

class NewPostBodyTest {

    private final NewPostBody body = new NewPostBody();

    private NewPostBody.Result read(byte[] json) {
        return body.read(json, 0, json.length);
    }

    private NewPostBody.Result read(String json) {
        return read(json.getBytes(StandardCharsets.UTF_8));
    }

    private String text() {
        return new String(body.text(), body.start(), body.length(), StandardCharsets.UTF_8);
    }

    @Test
    void trimsLikeJavaScript() {
        assertEquals(NewPostBody.Result.OK, read("{\"body\":\" \\u00a0hello\\n\\u2028\"}"));
        assertEquals("hello", text());
    }

    @Test
    void countsCharactersNotBytes() {
        assertEquals(NewPostBody.Result.OK, read("{\"body\":\"" + "é".repeat(500) + "\"}"));
        assertEquals(NewPostBody.Result.TOO_LONG, read("{\"body\":\"" + "é".repeat(501) + "\"}"));
    }

    @Test
    void requiresANonBlankBodyString() {
        assertEquals(NewPostBody.Result.MISSING, read("{\"text\":\"x\"}"));
        assertEquals(NewPostBody.Result.MISSING, read("{\"body\":7}"));
        assertEquals(NewPostBody.Result.MISSING, read("{\"body\":\"  \\t \"}"));
        assertEquals(NewPostBody.Result.MISSING, read("[\"body\"]"));
        assertEquals(NewPostBody.Result.MALFORMED, read("{\"body\":"));
    }

    @Test
    void rejectsInvalidUtf8AnywhereInTheDocument() {
        byte[] json = "{\"body\":\"ok\",\"x\":\"?\"}".getBytes(StandardCharsets.US_ASCII);
        json[json.length - 3] = (byte) 0xFF;
        assertEquals(NewPostBody.Result.MALFORMED, read(json));
        assertEquals(NewPostBody.Result.MALFORMED, read("{\"body\":\"\\ud800\"}")); // a lone surrogate escape
    }
}
