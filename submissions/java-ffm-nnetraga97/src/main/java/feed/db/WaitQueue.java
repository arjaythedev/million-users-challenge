package feed.db;

/** FIFO of queries waiting for room on a Postgres connection: (request tag, query) pairs in two parallel rings. */
final class WaitQueue {

    private long[] tags = new long[64];
    private byte[] queries = new byte[64];
    private int head;
    private int size;

    boolean isEmpty() {
        return size == 0;
    }

    int size() {
        return size;
    }

    void add(long tag, int query) {
        if (size == tags.length) {
            grow();
        }
        int i = (head + size) & (tags.length - 1);
        tags[i] = tag;
        queries[i] = (byte) query;
        size++;
    }

    long headTag() {
        return tags[head];
    }

    int headQuery() {
        return queries[head];
    }

    void removeHead() {
        head = (head + 1) & (tags.length - 1);
        size--;
    }

    private void grow() {
        long[] t = new long[tags.length * 2];
        byte[] q = new byte[tags.length * 2];
        for (int i = 0; i < size; i++) {
            int from = (head + i) & (tags.length - 1);
            t[i] = tags[from];
            q[i] = queries[from];
        }
        tags = t;
        queries = q;
        head = 0;
    }
}
