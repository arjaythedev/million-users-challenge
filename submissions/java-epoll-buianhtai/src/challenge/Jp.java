package challenge;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/** A small strict JSON parser: objects are Map, arrays List, numbers Double, plus String, Boolean and null. */
final class Jp {
    static final class Bad extends RuntimeException {
        Bad() {
            super("bad json", null, false, false);
        }
    }

    private static final Bad BAD = new Bad();

    private final byte[] b;
    private final int end;
    private int p;

    private Jp(byte[] b, int off, int len) {
        this.b = b;
        this.p = off;
        this.end = off + len;
    }

    static Object parse(byte[] b, int off, int len) {
        Jp j = new Jp(b, off, len);
        Object v = j.value(0);
        j.ws();
        if (j.p != j.end) throw BAD;
        return v;
    }

    private void ws() {
        while (p < end && (b[p] == ' ' || b[p] == '\t' || b[p] == '\n' || b[p] == '\r')) p++;
    }

    private Object value(int depth) {
        if (depth > 32) throw BAD;
        ws();
        if (p >= end) throw BAD;
        switch (b[p]) {
            case '{': {
                p++;
                Map<String, Object> m = new HashMap<>();
                ws();
                if (p < end && b[p] == '}') {
                    p++;
                    return m;
                }
                while (true) {
                    ws();
                    if (p >= end || b[p] != '"') throw BAD;
                    String k = string();
                    ws();
                    if (p >= end || b[p++] != ':') throw BAD;
                    m.put(k, value(depth + 1));
                    ws();
                    if (p >= end) throw BAD;
                    if (b[p] == ',') {
                        p++;
                    } else if (b[p] == '}') {
                        p++;
                        return m;
                    } else {
                        throw BAD;
                    }
                }
            }
            case '[': {
                p++;
                List<Object> l = new ArrayList<>();
                ws();
                if (p < end && b[p] == ']') {
                    p++;
                    return l;
                }
                while (true) {
                    l.add(value(depth + 1));
                    ws();
                    if (p >= end) throw BAD;
                    if (b[p] == ',') {
                        p++;
                    } else if (b[p] == ']') {
                        p++;
                        return l;
                    } else {
                        throw BAD;
                    }
                }
            }
            case '"':
                return string();
            case 't':
                return literal("true", Boolean.TRUE);
            case 'f':
                return literal("false", Boolean.FALSE);
            case 'n':
                return literal("null", null);
            default:
                return number();
        }
    }

    private Object literal(String s, Object v) {
        int n = s.length();
        if (end - p < n) throw BAD;
        for (int i = 0; i < n; i++) if (b[p + i] != s.charAt(i)) throw BAD;
        p += n;
        return v;
    }

    private Object number() {
        int s = p;
        if (p < end && b[p] == '-') p++;
        if (p >= end) throw BAD;
        if (b[p] == '0') {
            p++;
        } else {
            if (b[p] < '1' || b[p] > '9') throw BAD;
            while (p < end && b[p] >= '0' && b[p] <= '9') p++;
        }
        if (p < end && b[p] == '.') {
            p++;
            int digits = p;
            while (p < end && b[p] >= '0' && b[p] <= '9') p++;
            if (digits == p) throw BAD;
        }
        if (p < end && (b[p] == 'e' || b[p] == 'E')) {
            p++;
            if (p < end && (b[p] == '+' || b[p] == '-')) p++;
            int digits = p;
            while (p < end && b[p] >= '0' && b[p] <= '9') p++;
            if (digits == p) throw BAD;
        }
        try {
            return Double.parseDouble(new String(b, s, p - s, StandardCharsets.US_ASCII));
        } catch (NumberFormatException e) {
            throw BAD;
        }
    }

    private String string() {
        p++;
        StringBuilder sb = null;
        int run = p;
        while (true) {
            if (p >= end) throw BAD;
            byte c = b[p];
            if (c == '"') {
                String tail = new String(b, run, p - run, StandardCharsets.UTF_8);
                p++;
                return sb == null ? tail : sb.append(tail).toString();
            }
            if (c >= 0 && c < 0x20) throw BAD;
            if (c != '\\') {
                p++;
                continue;
            }
            if (sb == null) sb = new StringBuilder();
            sb.append(new String(b, run, p - run, StandardCharsets.UTF_8));
            p++;
            if (p >= end) throw BAD;
            switch (b[p++]) {
                case '"' -> sb.append('"');
                case '\\' -> sb.append('\\');
                case '/' -> sb.append('/');
                case 'b' -> sb.append('\b');
                case 'f' -> sb.append('\f');
                case 'n' -> sb.append('\n');
                case 'r' -> sb.append('\r');
                case 't' -> sb.append('\t');
                case 'u' -> {
                    if (end - p < 4) throw BAD;
                    int v = 0;
                    for (int i = 0; i < 4; i++) {
                        int d = Character.digit(b[p + i], 16);
                        if (d < 0) throw BAD;
                        v = v * 16 + d;
                    }
                    p += 4;
                    sb.append((char) v);
                }
                default -> throw BAD;
            }
            run = p;
        }
    }
}
