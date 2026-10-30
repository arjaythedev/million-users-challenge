"""GC policies: survivors leave the generations; young cycles are still collected."""
import subprocess
import sys
import unittest

PROBE = """
import gc, sys
from feed.runtime import memory
getattr(memory, sys.argv[1])()

class Node:
    pass

def cycle():
    a, b = Node(), Node()
    a.peer, b.peer = b, a
    return a

kept = [cycle() for _ in range(1000)]     # live: should leave the collected generations
for _ in range(1000):
    cycle()                               # garbage, young
freed = gc.collect(0)
young_and_middle = len(gc.get_objects(0)) + len(gc.get_objects(1))
nodes = sum(isinstance(o, Node) for g in (0, 1, 2) for o in gc.get_objects(g))
print(young_and_middle, nodes, freed)
"""


def probe(policy: str) -> list[str]:
    out = subprocess.run([sys.executable, "-c", PROBE, policy], capture_output=True, text=True, check=True)
    return out.stdout.split()


class GcPolicyTest(unittest.TestCase):
    def test_survivors_are_frozen_and_young_garbage_collected(self):
        middle, nodes, freed = probe("survivors")
        self.assertLess(int(middle), 50)       # nothing left for a gen-1 scan to traverse
        self.assertEqual(int(nodes), 0)        # the live cycles were frozen, not scanned
        self.assertGreaterEqual(int(freed), 2000)  # the garbage cycles were still collected

    def test_generational_keeps_survivors_in_the_generations(self):
        middle, nodes, freed = probe("generational")
        self.assertGreaterEqual(int(nodes), 2000)  # live cycles still there to be rescanned
        self.assertGreaterEqual(int(freed), 2000)


if __name__ == "__main__":
    unittest.main()
