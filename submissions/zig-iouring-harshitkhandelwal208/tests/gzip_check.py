#!/usr/bin/env python3
"""Round-trip the project's gzip encoder through Python's gzip/zlib on many inputs; report ratio against zlib."""
import gzip, os, random, subprocess, sys, zlib, json
TOOL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "zig-out", "bin", "deflate-tool")
def enc(b): return subprocess.run([TOOL], input=b, capture_output=True).stdout
random.seed(5)
cases = [b"", b"a", b"ab", b"abc", b"a" * 7, b"a" * 8, b"a" * 9, b"a" * 1000, b"abcabcabc" * 500, bytes(range(256)) * 4,
         os.urandom(1), os.urandom(100), os.urandom(5000), os.urandom(70000), b"x" * 300000]
words = "the a an and but or so because while after before just really very kind of sort of maybe server database request latency".split()
for n in (1, 5, 50, 300, 3000, 40000):
    cases.append((" ".join(random.choice(words) for _ in range(n))).encode())
for f in sys.argv[1:]:
    cases.append(open(f, "rb").read())
bad = 0
for c in cases:
    g = enc(c)
    try:
        ok = gzip.decompress(g) == c and zlib.decompress(g, 31) == c
    except Exception as e:
        ok = False
    if not ok:
        bad += 1; print("FAIL len=%d" % len(c))
print("cases:", len(cases), "failures:", bad)
for f in sys.argv[1:]:
    raw = open(f, "rb").read().rstrip(b"\n")
    z = zlib.compressobj(1, zlib.DEFLATED, 31); zl = len(z.compress(raw) + z.flush())
    print(os.path.basename(f), "raw", len(raw), "ours", len(enc(raw)), "zlib-1", zl)
sys.exit(1 if bad else 0)
