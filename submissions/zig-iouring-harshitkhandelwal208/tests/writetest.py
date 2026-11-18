#!/usr/bin/env python3
"""Concurrent writes: many users post and like at once; every response and the final counts are verified.

Run on a fresh copy of the seed. Usage: writetest.py [port] [users]. With the server started as TEST_FAIL_GROUPS=1 every
multi-member write group is poisoned, so the retry-one-by-one path is what gets tested.
"""
import base64, hashlib, hmac, json, sys, threading, time, http.client

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
N = int(sys.argv[2]) if len(sys.argv) > 2 else 300
SECRET = b"million-users-challenge"
fails = []


def b64(b):
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def token(uid):
    now = int(time.time())
    h = b64(b'{"alg":"HS256","typ":"JWT"}')
    p = b64(json.dumps({"sub": str(uid), "username": f"writer_{uid}", "iat": now, "exp": now + 3600}).encode())
    return f"{h}.{p}." + b64(hmac.new(SECRET, f"{h}.{p}".encode(), hashlib.sha256).digest())


def call(method, path, uid=None, body=None):
    c = http.client.HTTPConnection("127.0.0.1", PORT, timeout=30)
    hdr = {"Content-Type": "application/json"}
    if uid:
        hdr["Authorization"] = "Bearer " + token(uid)
    c.request(method, path, body=json.dumps(body) if body is not None else None, headers=hdr)
    r = c.getresponse()
    data = r.read()
    c.close()
    try:
        return r.status, json.loads(data)
    except Exception:
        return r.status, data


def fail(msg):
    fails.append(msg)
    print("FAIL", msg)


# the post everybody likes (new, so it starts at zero)
st, p = call("POST", "/posts", 1, {"body": "like target"})
target = p["post"]["id"]
created = {}
lock = threading.Lock()
barrier = threading.Barrier(N)


def worker(i):
    uid = 1000 + i
    barrier.wait()
    st, r = call("POST", "/posts", uid, {"body": f"post by {uid}"})
    if st != 201 or r["post"]["author"] != f"writer_{uid}" or r["post"]["like_count"] != 0:
        return fail(f"create {uid}: {st} {r}")
    with lock:
        created[uid] = r["post"]
    st, r = call("POST", f"/posts/{target}/like", uid)
    if st != 201 or r != {"liked": True, "already_liked": False, "post_id": target}:
        fail(f"first like {uid}: {st} {r}")
    st, r = call("POST", f"/posts/{target}/like", uid)
    if st != 200 or r != {"liked": True, "already_liked": True, "post_id": target}:
        fail(f"repeat like {uid}: {st} {r}")
    if i % 25 == 0:  # a like for a post that does not exist, in the middle of group commits
        st, r = call("POST", "/posts/987654321/like", uid)
        if st != 404 or r != {"error": "post not found"}:
            fail(f"like missing post {uid}: {st} {r}")


ts = [threading.Thread(target=worker, args=(i,)) for i in range(N)]
t0 = time.time()
[t.start() for t in ts]
[t.join() for t in ts]
print(f"{N} users x (post + 2 likes) in {time.time() - t0:.2f}s")

# the same user liking the same post from many connections at once: exactly one 201
res = []
b2 = threading.Barrier(40)


def dup():
    b2.wait()
    res.append(call("POST", f"/posts/{created[1000]['id']}/like", 5)[0])


ts = [threading.Thread(target=dup) for _ in range(40)]
[t.start() for t in ts]
[t.join() for t in ts]
if sorted(res) != [200] * 39 + [201]:
    fail(f"duplicate concurrent likes: {sorted(res)}")

st, r = call("GET", f"/posts/{target}")
if r["post"]["like_count"] != N:
    fail(f"like_count of target is {r['post']['like_count']}, want {N}")
for uid, post in created.items():
    st, r = call("GET", f"/posts/{post['id']}")
    want = dict(post)
    want.pop("author")  # the create response echoes the token's username; a read returns users.username
    got = dict(r.get("post", {})) if st == 200 else {}
    got.pop("author", None)
    if uid == 1000:
        want["like_count"] = 1
    if st != 200 or got != want:
        fail(f"read back {uid}: {st} {got} vs {want}")
ids = sorted(p["id"] for p in created.values())
if len(set(ids)) != N or ids != list(range(ids[0], ids[0] + N)):
    print("note: ids are not contiguous (other writers or rolled-back groups burn sequence values)")
st, feed = call("GET", "/feed")
# created_at is shown truncated to milliseconds while the database orders by microseconds (then id), so only the
# millisecond order can be checked here
stamps = [p["created_at"] for p in feed["posts"]]
if stamps != sorted(stamps, reverse=True):
    fail("feed is not newest first")

print(f"{len(fails)} failures")
sys.exit(1 if fails else 0)
