// Feed API (SPEC.md): an HTTP/1.1 server on one epoll loop per core, each loop with its own pipelined Postgres
// connections. Config comes only from DATABASE_URL, JWT_SECRET, HOST, PORT and EXTRA_PORT.
using System.Net;
using System.Runtime;
using System.Text;
using FeedServer;

var env = Environment.GetEnvironmentVariables();
string? Setting(string key) => env[key] is string { Length: > 0 } value ? value : null;
string Required(string key) => Setting(key) ?? throw new InvalidOperationException($"{key} is not set");

Pg.Configure(Required("DATABASE_URL"));
Jwt.Key = Encoding.UTF8.GetBytes(Required("JWT_SECRET"));
string host = Setting("HOST") ?? "127.0.0.1";
int[] ports = [.. new[] { Setting("PORT") ?? "3000", Setting("EXTRA_PORT") }.OfType<string>().Select(int.Parse)];

// One loop per core we may run on. Postgres connections: 64 for reads and 16 for writes in total, split evenly over
// the loops (rounded up), whatever this box's cores: the database box has 16 cores either way.
const int ReadPool = 64, WritePool = 16;
int loopCount = Environment.ProcessorCount; // honours the affinity mask
int readPerLoop = (ReadPool + loopCount - 1) / loopCount;
int writePerLoop = (WritePool + loopCount - 1) / loopCount;

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

Log.Info($"listening on {host}:{string.Join(",", ports)} loops={loopCount} fd_limit={fdLimit} " +
         $"pg_conns_per_loop=read:{readPerLoop}+write:{writePerLoop} " +
         $"gzip={(Gzip.HasLibdeflate ? "libdeflate" : "zlib-ng")} server_gc={GCSettings.IsServerGC} " +
         $"runtime={Environment.Version}");

Thread.Sleep(Timeout.Infinite);
