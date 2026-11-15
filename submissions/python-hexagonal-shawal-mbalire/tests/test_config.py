import os
import tempfile
import unittest

from feed.runtime.config import available_cores


class CgroupQuotaTest(unittest.TestCase):
    """available_cores reads the quota from the process's own cgroup and its ancestors, not the root."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = os.path.join(self.tmp.name, "cgroup")
        self.proc = os.path.join(self.tmp.name, "proc-self-cgroup")
        self.affinity = len(os.sched_getaffinity(0))

    def tearDown(self):
        self.tmp.cleanup()

    def cgroup(self, path: str, cpu_max: dict[str, str]):
        with open(self.proc, "w") as f:
            f.write(f"0::{path}\n")
        for rel, value in cpu_max.items():
            d = os.path.join(self.root, rel)
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "cpu.max"), "w") as f:
                f.write(value + "\n")

    def cores(self):
        return available_cores(self.proc, self.root)

    def test_quota_on_own_cgroup(self):
        self.cgroup("/system.slice/app.service", {"system.slice/app.service": "200000 100000"})
        self.assertEqual(self.cores(), min(self.affinity, 2))

    def test_tightest_ancestor_wins_and_rounds_up(self):
        self.cgroup("/a.slice/b.service", {"a.slice": "150000 100000", "a.slice/b.service": "max 100000"})
        self.assertEqual(self.cores(), min(self.affinity, 2))

    def test_no_quota_anywhere(self):
        self.cgroup("/a.slice/b.service", {"a.slice/b.service": "max 100000"})
        self.assertEqual(self.cores(), self.affinity)

    def test_root_cgroup_and_missing_files(self):
        self.cgroup("/", {})
        self.assertEqual(self.cores(), self.affinity)
        self.assertEqual(available_cores(os.path.join(self.tmp.name, "absent"), self.root), self.affinity)

    def test_tiny_quota_is_one_core(self):
        self.cgroup("/x", {"x": "10000 100000"})
        self.assertEqual(self.cores(), 1)


if __name__ == "__main__":
    unittest.main()
