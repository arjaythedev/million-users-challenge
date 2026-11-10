package feed.net;

import java.util.Arrays;

/**
 * fd to slot for one worker's connections: open addressing with linear probing over two int arrays, no boxing.
 * fds are process-wide, so a worker can't index its own arrays by fd without sizing them for every worker's
 * connections; this map lets it keep dense per-worker slots instead. Removal shifts the probe chain back (no
 * tombstones), so lookups stay short under churn.
 */
public final class FdMap {

    private static final int EMPTY = -1;

    private int[] keys;
    private int[] values;
    private int mask;
    private int size;

    public FdMap(int capacity) {
        int n = Integer.highestOneBit(Math.max(4, capacity) * 2 - 1);
        keys = new int[n];
        values = new int[n];
        Arrays.fill(keys, EMPTY);
        mask = n - 1;
    }

    /** The slot for {@code fd}, or -1. */
    public int get(int fd) {
        int i = index(fd);
        while (true) {
            int k = keys[i];
            if (k == fd) {
                return values[i];
            }
            if (k == EMPTY) {
                return -1;
            }
            i = (i + 1) & mask;
        }
    }

    public void put(int fd, int slot) {
        if ((size + 1) * 2 > keys.length) {
            grow();
        }
        int i = index(fd);
        while (keys[i] != EMPTY && keys[i] != fd) {
            i = (i + 1) & mask;
        }
        if (keys[i] == EMPTY) {
            size++;
        }
        keys[i] = fd;
        values[i] = slot;
    }

    public void remove(int fd) {
        int i = index(fd);
        while (keys[i] != fd) {
            if (keys[i] == EMPTY) {
                return;
            }
            i = (i + 1) & mask;
        }
        size--;
        // Shift later entries of the probe chain back into the hole.
        int hole = i;
        int j = i;
        while (true) {
            j = (j + 1) & mask;
            int k = keys[j];
            if (k == EMPTY) {
                break;
            }
            int home = index(k);
            boolean movable = hole <= j ? (home <= hole || home > j) : (home <= hole && home > j);
            if (movable) {
                keys[hole] = k;
                values[hole] = values[j];
                hole = j;
            }
        }
        keys[hole] = EMPTY;
    }

    public int size() {
        return size;
    }

    private int index(int fd) {
        int h = fd * 0x9E3779B9;
        return (h ^ (h >>> 16)) & mask;
    }

    private void grow() {
        int[] oldKeys = keys;
        int[] oldValues = values;
        keys = new int[oldKeys.length * 2];
        values = new int[oldKeys.length * 2];
        Arrays.fill(keys, EMPTY);
        mask = keys.length - 1;
        size = 0;
        for (int i = 0; i < oldKeys.length; i++) {
            if (oldKeys[i] != EMPTY) {
                put(oldKeys[i], oldValues[i]);
            }
        }
    }
}
