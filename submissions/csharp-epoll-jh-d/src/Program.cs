// Feed API (SPEC.md): an HTTP/1.1 server on one epoll loop per core, each loop with its own pipelined Postgres
// connections. Config comes only from DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT; the optional knobs are below.
using System.Net;
using System.Runtime;
using System.Text;
using FeedServer;

var env = Environment.GetEnvironmentVariables();
string? Setting(string key) => env[key] is string { Length: > 0 } value ? value : null;
string Required(string key) => Setting(key) ?? throw new InvalidOperationException($"{key} is not set");
int Knob(string key, int fallback) => int.TryParse(Setting(key), out int n) && n >= 0 ? n : fallback;

Pg.Configure(Required("DATABASE_URL"));
Jwt.Key = Encoding.UTF8.GetBytes(Required("JWT_SECRET"));
string host = Setting("HOST") ?? "127.0.0.1";
int[] ports = [.. new[] { Setting("PORT") ?? "3000", Setting("EXTRA_PORT") }.OfType<string>().Select(int.Parse)];

// Optional knobs: LOOPS (default: the cores we may run on), DB_READ_POOL / DB_WRITE_POOL (Postgres connections in
// total, split evenly over the loops; the database box has 16 cores whatever this box has), BATCH_US (batching
// window), DB_DEAL (target requests per Postgres round trip), PG_TIMEOUT_MS
// (a silent Postgres connection owing answers is dropped after this), STATS_SEC (a stats line per loop; 0: never),
// GZIP_LEVEL.
int cores = Environment.ProcessorCount; // honours the affinity mask
int loopCount = Math.Max(1, Knob("LOOPS", cores));
int readPerLoop = Math.Max(1, (Math.Clamp(Knob("DB_READ_POOL", 64), 1, 400) + loopCount - 1) / loopCount);
int writePerLoop = Math.Max(1, (Math.Clamp(Knob("DB_WRITE_POOL", 16), 1, 100) + loopCount - 1) / loopCount);
if ((readPerLoop + writePerLoop) * loopCount > 500) throw new InvalidOperationException("more than 500 Postgres connections");
EventLoop.BatchMicros = Knob("BATCH_US", 2000);
EventLoop.DealSize = Math.Max(1, Knob("DB_DEAL", 16));
EventLoop.PgTimeoutMs = Math.Max(1000, Knob("PG_TIMEOUT_MS", 10_000));
EventLoop.StatsSeconds = Knob("STATS_SEC", 10);
Gzip.Level = Math.Clamp(Knob("GZIP_LEVEL", 1), 1, 12);

uint addr = host switch
{
    "0.0.0.0" or "*" or "" => 0,
    "localhost" => BitConverter.ToUInt32(IPAddress.Loopback.GetAddressBytes()),
    _ => BitConverter.ToUInt32(IPAddress.Parse(host).GetAddressBytes()),
};

int fdLimit = Native.RaiseFdLimit();
EventLoop.Init(fdLimit);

// Each loop runs on its own thread from Start(); this thread only waits.
EventLoop[] loops = [.. Enumerable.Range(0, loopCount).Select(id => new EventLoop(id, addr, ports, readPerLoop, writePerLoop))];
Array.ForEach(loops, l => l.Start());

Log.Info($"listening on {host}:{string.Join(",", ports)} cores={cores} loops={loopCount} fd_limit={fdLimit} " +
         $"pg_conns_per_loop=read:{readPerLoop}+write:{writePerLoop} db_deal={EventLoop.DealSize} " +
         $"batch_us={EventLoop.BatchMicros} stats_sec={EventLoop.StatsSeconds} " +
         $"gzip={(Gzip.HasLibdeflate ? "libdeflate" : "zlib-ng")}:{Gzip.Level} server_gc={GCSettings.IsServerGC} " +
         $"runtime={Environment.Version}");

Thread.Sleep(Timeout.Infinite);
