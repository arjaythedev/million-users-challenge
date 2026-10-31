package feed.http;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;

import org.junit.jupiter.api.Test;

class BytesTest {

    private static byte[] b(String s) {
        return s.getBytes(StandardCharsets.UTF_8);
    }

    private static boolean utf8(int... bytes) {
        byte[] b = new byte[bytes.length];
        for (int i = 0; i < bytes.length; i++) {
            b[i] = (byte) bytes[i];
        }
        return Bytes.isValidUtf8(b, 0, b.length);
    }

    @Test
    void equalsComparesARangeWithAString() {
        byte[] x = b("GET /feed");
        assertTrue(Bytes.equals(x, 4, 9, "/feed"));
        assertFalse(Bytes.equals(x, 4, 8, "/feed"));
        assertFalse(Bytes.equals(x, 4, 9, "/Feed"));
    }

    @Test
    void indexOfFindsAByteInARange() {
        byte[] x = b("a.b.c");
        assertEquals(1, Bytes.indexOf(x, 0, 5, (byte) '.'));
        assertEquals(3, Bytes.indexOf(x, 2, 5, (byte) '.'));
        assertEquals(-1, Bytes.indexOf(x, 4, 5, (byte) '.'));
    }

    @Test
    void positiveIdAcceptsOnlyPositiveDecimalsOfAtMost18Digits() {
        assertEquals(42, Bytes.positiveId(b("x42"), 1, 3));
        assertEquals(7, Bytes.positiveId(b("007"), 0, 3));
        assertEquals(999_999_999_999_999_999L, Bytes.positiveId(b("999999999999999999"), 0, 18));
        assertEquals(0, Bytes.positiveId(b("0"), 0, 1));
        assertEquals(0, Bytes.positiveId(b(""), 0, 0));
        assertEquals(0, Bytes.positiveId(b("-1"), 0, 2));
        assertEquals(0, Bytes.positiveId(b("1.5"), 0, 3));
        assertEquals(0, Bytes.positiveId(b("1000000000000000000"), 0, 19));
    }

    @Test
    void acceptsWellFormedUtf8() {
        byte[] ok = b("plain café ✓ 😀");
        assertTrue(Bytes.isValidUtf8(ok, 0, ok.length));
        assertTrue(utf8());
    }

    @Test
    void rejectsMalformedUtf8() {
        assertFalse(utf8(0xF0, 'a'));             // truncated 4-byte sequence
        assertFalse(utf8(0xC3));                  // lead byte at the end
        assertFalse(utf8(0x80));                  // lone continuation byte
        assertFalse(utf8(0xC0, 0xAF));            // overlong '/'
        assertFalse(utf8(0xE0, 0x80, 0xAF));      // overlong
        assertFalse(utf8(0xED, 0xA0, 0x80));      // UTF-16 surrogate
        assertFalse(utf8(0xF4, 0x90, 0x80, 0x80)); // above U+10FFFF
        assertFalse(utf8(0xFF));
    }
}
