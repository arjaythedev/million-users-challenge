require "./sys"
require "./buf"
require "./json"
require "./jwt"
require "./http"
require "./net"
require "./uring"
require "./pg"
require "./app"

# One worker process per CPU. Each has its own SO_REUSEPORT listeners on both ports (the
# kernel spreads connections over them), its own io_uring and its own Postgres
# connections; they share nothing. This process only starts and watches them.
secret = Sys.env("JWT_SECRET") || Sys.die("JWT_SECRET is required")
url = Sys.env("DATABASE_URL") || Sys.die("DATABASE_URL is required")
host = Sys.env("HOST") || "0.0.0.0"
ports = {(Sys.env("PORT") || "3000").to_i, (Sys.env("EXTRA_PORT") || "3001").to_i}
workers = (Sys.env("WORKERS") || Sys.ncpu.to_s).to_i
# rule 5: at most 512 Postgres connections in all
per_worker = (500 // workers).clamp(1, 32)
Jwt.init(secret)
nofile = Sys.raise_nofile
nofile = 1_u64 << 21 if nofile > (1_u64 << 21)
started = Sys.now_ns(Sys::CLOCK_MONOTONIC) // 1_000_000_000

pids = Pointer(Int32).malloc(workers)
workers.times do |i|
  pid = LibC.fork
  if pid == 0
    LibSys.prctl(1, 15_u64) # PR_SET_PDEATHSIG = SIGTERM: go when the parent goes
    Server.init(started)
    Pg.connect_all(url, per_worker)
    lfds = Pointer(Int32).malloc(2)
    one = 1
    2.times do |k|
      lfds[k] = Sys.listen_tcp(host, ports[k], true)
      LibC.setsockopt(lfds[k], Sys::IPPROTO_TCP, Sys::TCP_NODELAY, pointerof(one).as(Void*), 4)
    end
    Sys.die("io_uring is not available (Linux 6.1+ needed)") unless Uring.setup(lfds, 2, nofile, workers)
    STDERR.puts "worker #{i}: #{host}:#{ports[0]},#{ports[1]}, #{per_worker} Postgres connections, #{Uring.describe}" if i == 0
    Uring.run
  end
  Sys.die("fork failed") if pid < 0
  pids[i] = pid
end

# A worker that dies takes the server down (the challenge doesn't restart crashed servers
# anyway): no half-capacity zombie.
status = 0
dead = LibC.waitpid(-1, pointerof(status), 0)
STDERR.puts "worker #{dead} exited (status #{status}), stopping"
workers.times { |i| LibC.kill(pids[i], 15) unless pids[i] == dead }
LibC.exit(1)
