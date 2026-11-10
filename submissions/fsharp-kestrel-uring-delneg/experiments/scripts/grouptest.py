# grouptest.py <port> <tokens.json> <dbname>: fires 150 creates, 75 likes on existing posts and 75 likes on a missing
# post at once over 300 connections (so they share loop passes and write batches); checks the statuses, and that
# every created post is in the database exactly once with the id it was answered with
import json, socket, sys, time, subprocess, random
port = int(sys.argv[1]); toks = json.load(open(sys.argv[2])); db = sys.argv[3]
random.seed(7)
reqs = []
tag = "grp-%d" % time.time_ns()
for i in range(150):
    t = toks[i % len(toks)]
    body = json.dumps({"body": "%s-%d" % (tag, i)})
    reqs.append(("create", i, "POST /posts HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s" % (t["token"], len(body), body)))
for i in range(75):
    t = toks[(i * 7 + 3) % len(toks)]
    reqs.append(("like", i, "POST /posts/%d/like HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Length: 0\r\n\r\n" % (1 + i * 13, t["token"])))
for i in range(75):
    t = toks[(i * 11 + 5) % len(toks)]
    reqs.append(("missing", i, "POST /posts/999999999/like HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Length: 0\r\n\r\n" % t["token"]))
random.shuffle(reqs)
socks = []
for _ in reqs:
    s = socket.create_connection(("127.0.0.1", port)); socks.append(s)
for s, (_, _, r) in zip(socks, reqs): s.sendall(r.encode())
res = {"create": [], "like": [], "missing": []}
ids = {}
for s, (kind, i, _) in zip(socks, reqs):
    s.settimeout(10); data = b""
    while b"\r\n\r\n" not in data: data += s.recv(65536)
    head, _, body = data.partition(b"\r\n\r\n")
    n = int([l for l in head.split(b"\r\n") if l.lower().startswith(b"content-length")][0].split(b":")[1])
    while len(body) < n: body += s.recv(65536)
    status = int(head.split(b" ")[1]); res[kind].append(status)
    if kind == "create" and status == 201: ids[i] = json.loads(body)["post"]["id"]
    s.close()
print("create", sorted(set(res["create"])), "like", sorted(set(res["like"])), "missing", sorted(set(res["missing"])))
rows = subprocess.run(["sudo", "-u", "postgres", "psql", "-qXtA", "-d", db, "-c",
    "SELECT id, body FROM posts WHERE body LIKE '%s-%%'" % tag], capture_output=True, text=True).stdout.split()
indb = {int(r.split("|")[1].rsplit("-", 1)[1]): int(r.split("|")[0]) for r in rows}
ok = (res["create"] == [201] * 150 and set(res["like"]) <= {200, 201} and res["missing"] == [404] * 75
      and indb == ids and len(indb) == 150)
print("db rows", len(indb), "answers match db", indb == ids)
print("GROUPTEST-OK" if ok else "GROUPTEST-FAIL")
