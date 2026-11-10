package feed.pg;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;

/** Builds the bytes a Postgres backend would send, for feeding PgProtocol in tests. */
final class Backend {

    private final ByteArrayOutputStream out = new ByteArrayOutputStream();

    Backend message(char type, byte[] body) {
        out.write(type);
        out.writeBytes(int4(body.length + 4));
        out.writeBytes(body);
        return this;
    }

    Backend authOk() {
        return message('R', int4(0));
    }

    Backend authCleartext() {
        return message('R', int4(3));
    }

    Backend authMd5(byte[] salt) {
        return message('R', concat(int4(5), salt));
    }

    Backend authSasl(String... mechanisms) {
        ByteArrayOutputStream body = new ByteArrayOutputStream();
        body.writeBytes(int4(10));
        for (String m : mechanisms) {
            body.writeBytes(cstring(m));
        }
        body.write(0);
        return message('R', body.toByteArray());
    }

    Backend authSaslContinue(String data) {
        return message('R', concat(int4(11), text(data)));
    }

    Backend authSaslFinal(String data) {
        return message('R', concat(int4(12), text(data)));
    }

    Backend parameterStatus(String name, String value) {
        return message('S', concat(cstring(name), cstring(value)));
    }

    Backend backendKeyData() {
        return message('K', concat(int4(1234), int4(5678)));
    }

    Backend notice(String text) {
        return message('N', concat(new byte[] {'M'}, cstring(text), new byte[] {0}));
    }

    Backend readyForQuery() {
        return message('Z', new byte[] {'I'});
    }

    Backend parseComplete() {
        return message('1', new byte[0]);
    }

    Backend bindComplete() {
        return message('2', new byte[0]);
    }

    /** A DataRow; a null column is sent as length -1. */
    Backend dataRow(byte[]... columns) {
        ByteArrayOutputStream body = new ByteArrayOutputStream();
        body.write(columns.length >> 8);
        body.write(columns.length);
        for (byte[] c : columns) {
            if (c == null) {
                body.writeBytes(int4(-1));
            } else {
                body.writeBytes(int4(c.length));
                body.writeBytes(c);
            }
        }
        return message('D', body.toByteArray());
    }

    Backend commandComplete(String tag) {
        return message('C', cstring(tag));
    }

    Backend error(String sqlState, String text) {
        return message('E', concat(new byte[] {'S'}, cstring("ERROR"), new byte[] {'C'}, cstring(sqlState),
                new byte[] {'M'}, cstring(text), new byte[] {0}));
    }

    /** Startup done on a trust connection: AuthenticationOk through ReadyForQuery. */
    Backend trustStartup() {
        return authOk().parameterStatus("server_version", "18.6").parameterStatus("client_encoding", "UTF8")
                .backendKeyData().readyForQuery();
    }

    /** Answers to preparing {@code n} statements (Parse x n, then Sync). */
    Backend prepared(int n) {
        for (int i = 0; i < n; i++) {
            parseComplete();
        }
        return readyForQuery();
    }

    byte[] bytes() {
        return out.toByteArray();
    }

    static byte[] int4(int v) {
        return ByteBuffer.allocate(4).putInt(v).array();
    }

    static byte[] int8(long v) {
        return ByteBuffer.allocate(8).putLong(v).array();
    }

    static byte[] text(String s) {
        return s.getBytes(StandardCharsets.UTF_8);
    }

    static byte[] cstring(String s) {
        return concat(text(s), new byte[] {0});
    }

    static byte[] concat(byte[]... parts) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        for (byte[] p : parts) {
            out.writeBytes(p);
        }
        return out.toByteArray();
    }
}
