package feed.pg;

import java.util.ArrayList;
import java.util.HexFormat;
import java.util.List;

/** A PgHandler that records every callback as a line of text. */
final class EventLog implements PgHandler {

    final List<String> events = new ArrayList<>();

    @Override
    public void onRow(long tag, PgRow row) {
        StringBuilder s = new StringBuilder("row ").append(tag);
        for (int i = 0; i < row.columnCount(); i++) {
            s.append(' ');
            if (row.isNull(i)) {
                s.append("null");
            } else {
                s.append(HexFormat.of().formatHex(row.buffer(), row.offset(i), row.offset(i) + row.length(i)));
            }
        }
        events.add(s.toString());
    }

    @Override
    public void onComplete(long tag, long rows) {
        events.add("complete " + tag + " " + rows);
    }

    @Override
    public void onError(long tag, PgError error) {
        events.add("error " + tag + " " + error.sqlState() + " " + error.message());
    }

    @Override
    public void onDone(long tag) {
        events.add("done " + tag);
    }
}
