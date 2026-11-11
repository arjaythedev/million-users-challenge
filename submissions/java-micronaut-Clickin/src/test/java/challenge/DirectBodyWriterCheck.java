package challenge;

import io.micronaut.core.type.Argument;
import io.micronaut.http.MediaType;
import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Objects;
import tools.jackson.core.json.JsonFactory;

public final class DirectBodyWriterCheck {
    public static void main(String[] args) {
        var post = new Responses.Post(42, "한😀\"\\\n\u0001", "2026-10-28T01:02:03.004Z", "a\\b", 7);
        String encodedPost = "{\"id\":42,\"body\":\"한😀\\\"\\\\\\n\\u0001\",\"created_at\":\"2026-10-28T01:02:03.004Z\",\"author\":\"a\\\\b\",\"like_count\":7}";
        check(new Responses.Single(post), "{\"post\":" + encodedPost + "}");
        check(new Responses.Feed(List.of(post)), "{\"posts\":[" + encodedPost + "]}");
        check(new Responses.Feed(List.of()), "{\"posts\":[]}");
    }

    private static void check(Object value, String expected) {
        var out = new ByteArrayOutputStream();
        DirectBodyWriter.INSTANCE.writeTo(Argument.of(Object.class), MediaType.APPLICATION_JSON_TYPE, value, null, out);
        var json = JsonFactory.builder().build();
        try (var actual = json.createParser(out.toByteArray()); var wanted = json.createParser(expected)) {
            while (true) {
                var token = actual.nextToken();
                if (token != wanted.nextToken() || (token != null && !Objects.equals(actual.getString(), wanted.getString())))
                    throw new AssertionError(out.toString(StandardCharsets.UTF_8));
                if (token == null) return;
            }
        }
    }
}
