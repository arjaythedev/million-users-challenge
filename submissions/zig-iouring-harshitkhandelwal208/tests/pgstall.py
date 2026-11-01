#!/usr/bin/env python3
"""Postgres that goes silent without a reset (a crashed or partitioned host): the server must notice, answer /health with
503 meanwhile, replace its connections, and serve again once the database is back.

A small TCP proxy sits between the server and Postgres and can be frozen: frozen, it keeps every socket open but forwards
nothing, which is what a vanished host looks like to the client. Usage:

    PGPASSWORD=... tests/pgstall.py <path to bin/feed> <database name> [pg host:port, default 127.0.0.1:5432]
"""
import os, socket, subprocess, sys, threading, time, urllib.request, urllib.error

BIN, DB = sys.argv[1], sys.argv[2]
PG = sys.argv[3] if len(sys.argv) > 3 else "127.0.0.1:5432"
PHOST, PPORT = PG.split(":")
PROXY_PORT = 5999
frozen = threading.Event()  # set = frozen
fails = 0


def pump(a, b):
    try:
        while True:
            while frozen.is_set():
                time.sleep(0.05)
            d = a.recv(65536)
            if not d:
                break
            while frozen.is_set():
                time.sleep(0.05)
            b.sendall(d)
    except OSError:
        pass
    for x in (a, b):
        try:
            x.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass


def proxy():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PROXY_PORT))
    srv.listen(256)
    while True:
        c, _ = srv.accept()
        try:
            u = socket.create_connection((PHOST, int(PPORT)))
        except OSError:
            c.close()
            continue
        threading.Thread(target=pump, args=(c, u), daemon=True).start()
        threading.Thread(target=pump, args=(u, c), daemon=True).start()


def get(path, timeout=30):
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:3000{path}", timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:
        return None, str(e).encode()


def check(name, ok, detail=""):
    global fails
    print(("PASS " if ok else "FAIL ") + name + ("" if ok else "  " + str(detail)))
    if not ok:
        fails += 1


threading.Thread(target=proxy, daemon=True).start()
env = dict(os.environ, DATABASE_URL=f"postgres://postgres:{os.environ.get('PGPASSWORD', 'postgres')}@127.0.0.1:{PROXY_PORT}/{DB}",
           JWT_SECRET="million-users-challenge", HOST="127.0.0.1", PORT="3000", EXTRA_PORT="3001")
srv = subprocess.Popen([BIN], env=env, stdout=subprocess.DEVNULL, stderr=open(os.environ.get("SERVER_LOG", os.devnull), "w"))
try:
    for _ in range(60):
        st, _b = get("/health", 2)
        if st == 200:
            break
        time.sleep(0.25)
    check("healthy through the proxy", st == 200, st)

    frozen.set()
    t0 = time.time()
    result = {}
    threading.Thread(target=lambda: result.update(zip(("st", "body"), get("/feed", 60)))).start()
    # traffic on every connection, as in a real run (an idle connection is only noticed by TCP keepalive, ~35 s)
    stop = threading.Event()

    def load():
        while not stop.is_set():
            get("/feed", 30)

    loaders = [threading.Thread(target=load, daemon=True) for _ in range(150)]
    for t in loaders:
        t.start()
    # within the in-flight deadline (10 s) plus a tick or two, health must say degraded instead of hanging
    deadline = t0 + 25
    seen_503 = False
    while time.time() < deadline:
        st, b = get("/health", 3)
        if st == 503:
            seen_503 = True
            break
        time.sleep(0.5)
    check(f"/health reports 503 degraded after the database went silent ({time.time() - t0:.1f}s)", seen_503, (st, b[:80]))

    stop.set()
    frozen.clear()
    ok = False
    t1 = time.time()
    while time.time() - t1 < 30:
        st, b = get("/health", 3)
        if st == 200:
            ok = True
            break
        time.sleep(0.5)
    check(f"/health is ok again after the database is back ({time.time() - t1:.1f}s)", ok, (st, b[:80]))
    st, b = get("/feed", 10)
    check("/feed works again", st == 200 and b.startswith(b'{"posts":['), (st, b[:60]))
    time.sleep(2)
    check("the request that was in flight during the stall was answered (200 retried, or 500 after its tries)", result.get("st") in (200, 500, 503), result)
finally:
    srv.terminate()
    srv.wait()
print(f"\n{fails} failures")
sys.exit(1 if fails else 0)
