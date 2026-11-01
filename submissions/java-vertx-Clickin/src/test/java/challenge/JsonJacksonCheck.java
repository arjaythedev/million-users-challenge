package challenge;

import java.io.ByteArrayOutputStream;
import java.util.Arrays;
import java.util.Random;
import tools.jackson.core.json.JsonFactory;
import tools.jackson.core.json.JsonWriteFeature;

public final class JsonJacksonCheck {
    private static final JsonFactory FACTORY = JsonFactory.builder().enable(JsonWriteFeature.COMBINE_UNICODE_SURROGATES_IN_UTF8).build();

    public static void main(String[] args) {
        var json = new Json();
        for (String value : new String[] {null, "", "x", "x".repeat(20_000), "😀", "한😀\"\\\n\ud800"}) checkString(json, value);
        for (String part : new String[] {"\u0000", "\b", "\n", "\u001f", "\"", "\\", "\u007f", "\u0080", "\u00ff", "\u0100", "\u0120", "\u0122", "\u0141", "\u017f", "\u0180", "한", "😀", "\ud800", "\udc00"}) {
            checkString(json, part);
            for (int pos : new int[] {0, 16, 32}) checkString(json, "x".repeat(pos) + part + "x".repeat(32 - pos));
        }
        for (int pos : new int[] {0, 10_000, 20_000}) checkString(new Json(), "x".repeat(pos) + "\u0141\"" + "x".repeat(20_000 - pos));
        checkString(new Json(), "\u0001".repeat(8190));
        for (int c = 0; c <= 0xffff; c++) checkString(json, "a" + (char) c + "z");
        var random = new Random(912308);
        for (int i = 0; i < 1000; i++) {
            var value = new StringBuilder();
            for (int j = 0, n = random.nextInt(100); j < n; j++) value.append((char) random.nextInt(0x10000));
            checkString(json, value.toString());
        }
        for (long value : new long[] {Long.MIN_VALUE, Long.MIN_VALUE + 1, -1000, -101, -100, -99, -10, -1, 0, 1, 9, 10, 99, 100, 101, 999, 1000, Long.MAX_VALUE}) checkNumber(json, value);
        for (long power = 1; power > 0; power = power <= Long.MAX_VALUE / 10 ? power * 10 : 0) {
            for (long value : new long[] {power - 1, power, power + 1}) { checkNumber(json, value); checkNumber(json, -value); }
        }
        for (int i = 0; i < 10_000; i++) checkNumber(json, random.nextLong());
        System.out.println("Jackson JSON byte comparison passed");
    }

    private static void checkString(Json json, String value) {
        var output = new ByteArrayOutputStream();
        try (var generator = FACTORY.createGenerator(output)) {
            generator.writeStartObject();
            generator.writeStringProperty("error", value);
            generator.writeEndObject();
        }
        json.error(value);
        check(json, output.toByteArray());
    }

    private static void checkNumber(Json json, long value) {
        var output = new ByteArrayOutputStream();
        try (var generator = FACTORY.createGenerator(output)) {
            generator.writeStartObject();
            generator.writeBooleanProperty("liked", true);
            generator.writeBooleanProperty("already_liked", false);
            generator.writeNumberProperty("post_id", value);
            generator.writeEndObject();
        }
        json.like(value, false);
        check(json, output.toByteArray());
    }

    private static void check(Json json, byte[] expected) {
        byte[] actual = Arrays.copyOf(json.bytes(), json.size());
        if (!Arrays.equals(actual, expected))
            throw new AssertionError("actual=" + new String(actual, java.nio.charset.StandardCharsets.UTF_8) + " expected=" + new String(expected, java.nio.charset.StandardCharsets.UTF_8));
    }
}
