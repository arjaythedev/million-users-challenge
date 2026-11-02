package feed.net;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.util.HashMap;
import java.util.Map;
import java.util.SplittableRandom;

import org.junit.jupiter.api.Test;

class FdMapTest {

    @Test
    void putGetRemove() {
        FdMap m = new FdMap(4);
        assertEquals(-1, m.get(7));
        m.put(7, 0);
        m.put(8, 1);
        assertEquals(0, m.get(7));
        assertEquals(1, m.get(8));
        m.put(7, 5);
        assertEquals(5, m.get(7));
        assertEquals(2, m.size());
        m.remove(7);
        assertEquals(-1, m.get(7));
        assertEquals(1, m.get(8));
        assertEquals(1, m.size());
        m.remove(99);
        assertEquals(1, m.size());
    }

    @Test
    void growsPastItsInitialCapacity() {
        FdMap m = new FdMap(2);
        for (int fd = 0; fd < 100_000; fd++) {
            m.put(fd, fd * 3);
        }
        for (int fd = 0; fd < 100_000; fd++) {
            assertEquals(fd * 3, m.get(fd));
        }
        assertEquals(100_000, m.size());
    }

    /** Random inserts and removals, checked against HashMap (exercises removal inside collision chains). */
    @Test
    void agreesWithHashMapUnderChurn() {
        FdMap m = new FdMap(16);
        Map<Integer, Integer> reference = new HashMap<>();
        SplittableRandom random = new SplittableRandom(7);
        for (int i = 0; i < 500_000; i++) {
            int fd = random.nextInt(5_000);
            if (random.nextInt(3) == 0) {
                m.remove(fd);
                reference.remove(fd);
            } else {
                m.put(fd, i);
                reference.put(fd, i);
            }
            int probe = random.nextInt(5_000);
            assertEquals(reference.getOrDefault(probe, -1), m.get(probe), "step " + i + " fd " + probe);
        }
        assertEquals(reference.size(), m.size());
    }
}
