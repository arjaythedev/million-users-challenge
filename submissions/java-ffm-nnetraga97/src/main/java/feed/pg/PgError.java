package feed.pg;

import java.nio.charset.StandardCharsets;

/**
 * A view over one {@code ErrorResponse} (or a connection failure made up by the driver). Reused: valid only in the
 * callback. {@link #is} compares without allocating; the String accessors allocate (error paths only).
 */
public final class PgError {

    private byte[] buffer;
    private int stateOff;
    private int stateLen;
    private int messageOff;
    private int messageLen;
    private String syntheticState;
    private String syntheticMessage;

    PgError() {
    }

    /** Points the view at an ErrorResponse body: (field type byte, C string) pairs, ended by a 0 byte. */
    void reset(byte[] buf, int off) {
        buffer = buf;
        syntheticState = null;
        syntheticMessage = null;
        stateLen = 0;
        messageLen = 0;
        int pos = off;
        while (buf[pos] != 0) {
            byte field = buf[pos++];
            int start = pos;
            while (buf[pos] != 0) {
                pos++;
            }
            if (field == 'C') {
                stateOff = start;
                stateLen = pos - start;
            } else if (field == 'M') {
                messageOff = start;
                messageLen = pos - start;
            }
            pos++;
        }
    }

    void synthetic(String sqlState, String message) {
        syntheticState = sqlState;
        syntheticMessage = message;
    }

    /** True if the SQLSTATE is {@code code}, e.g. {@code "23503"}. */
    public boolean is(String code) {
        if (syntheticState != null) {
            return syntheticState.equals(code);
        }
        if (code.length() != stateLen) {
            return false;
        }
        for (int i = 0; i < stateLen; i++) {
            if (buffer[stateOff + i] != code.charAt(i)) {
                return false;
            }
        }
        return true;
    }

    public String sqlState() {
        return syntheticState != null ? syntheticState
                : new String(buffer, stateOff, stateLen, StandardCharsets.US_ASCII);
    }

    public String message() {
        return syntheticMessage != null ? syntheticMessage
                : new String(buffer, messageOff, messageLen, StandardCharsets.UTF_8);
    }
}
