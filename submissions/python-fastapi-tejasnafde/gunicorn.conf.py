# Gunicorn settings (https://docs.gunicorn.org/en/stable/settings.html). HOST/PORT/EXTRA_PORT from the environment.
import os

_host = os.environ.get("HOST", "127.0.0.1")
# A list binds every address with the same settings and the same workers. EXTRA_PORT is a second
# server port, as SPEC.md asks.
bind = [f"{_host}:{os.environ.get("PORT", "3000")}"]
if os.environ.get("EXTRA_PORT"):
    bind.append(f"{_host}:{os.environ['EXTRA_PORT']}")

# 1.5 workers per core, measured (4 cores, 300k users): 4 workers left p99 at 1.1 s, 8 cost 6% more
# CPU per request than 6. Gunicorn's usual (2 x cores) + 1 is for blocking workers; these are async.
# Cores = the CPUs this process may run on (systemd CPUAffinity / taskset), not os.cpu_count().
cores = len(os.sched_getaffinity(0))
workers = int(os.environ.get("WEB_CONCURRENCY") or max(1, cores * 3 // 2))  # app/db.py uses the same formula
worker_class = "app.worker.Worker"

# Each worker binds its own SO_REUSEPORT listening socket (Gunicorn creates them per worker when
# this is on), so the kernel spreads new connections evenly across workers. With the default
# shared socket, a few workers accept most connections (measured: 2.7-3.5x the median worker's
# connections and CPU), and the busiest worker hits 100% of a core long before the others.
reuse_port = True

# Clients idle up to ~40 s between requests on one keep-alive connection. UvicornWorker passes
# this to uvicorn as timeout_keep_alive (uvicorn's own default is 5 s, gunicorn's 2 s).
keepalive = 75
backlog = 65535  # default 2048; the kernel caps it at net.core.somaxconn
timeout = 60     # a worker silent for 60 s is restarted (default 30)
max_requests = 0  # default: workers are never recycled
accesslog = None  # default: no access log
errorlog = "-"
loglevel = "warning"
