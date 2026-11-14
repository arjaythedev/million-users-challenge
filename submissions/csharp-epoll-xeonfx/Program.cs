using System;
using System.Text;
using System.Threading;

// Each epoll loop has its own listeners and Postgres connections.
Loop.StartedAt = Environment.TickCount64;
Loop.Jwt = new Jwt(Encoding.UTF8.GetBytes(Environment.GetEnvironmentVariable("JWT_SECRET") ?? "million-users-challenge"));
Loop.Conns = new Conn[Native.MaxFiles()];

var db = PgSettings.Parse(Environment.GetEnvironmentVariable("DATABASE_URL") ?? "postgres://postgres@localhost:5432/feed");
int[] ports = [Port("PORT", 3000), Port("EXTRA_PORT", 3001)];
// One loop per core from 4 cores up. One fewer saved CPU on a CPU-bound test box, but the scoring box runs out of
// network before CPU, and there it held fewer users: 1,360,000 on 4 cores with 3 loops against 1,390,000 with 4
// (fsharp-kestrel-uring lost the same way, 1,350,000 against 1,510,000).
// Four at least on 2 and 3: the NIC has one queue, so its interrupts and most of the receive softirq land on one CPU
// (on 2 vCPUs at 1,200,000 users, 0.53 cores of softirq on one against 0.23 on the other). With one loop per CPU, the
// loop that shares that CPU falls behind while the other CPU idles, and the whole server fails when that one CPU is
// full. With more loops than CPUs the scheduler moves them off the busy one: on a 2-vCPU test box the two CPUs went
// from 90 % and 58 % busy to 77 % and 70 % at the same total, and 1,400,000 users passed where 1,300,000 failed. One
// CPU has nowhere to move a loop to, so it keeps one.
int cores = Environment.ProcessorCount;
int loops = cores == 1 ? 1 : Math.Max(cores, 4);
// a pass's reads go out as one batch on one reader, so a loop needs only as many readers as batches it can have out
// at once (a backend is one process, one core, and the database has 16): about 32 in all, 2 to 8 a loop. A pass's
// writes go as one group commit on a writer with none out: 2 to 4 a loop. That makes 48 connections on 2 and 4
// vCPUs, 48 on 8 and 64 on 16, far under Postgres's 512
int readers = Math.Clamp(32 / loops, 2, 8), writers = Math.Clamp(16 / loops, 2, 4);
// more loops than that leaves room for: each keeps one reader and one writer at least, within 480 connections
while (loops * (readers + writers) > 480 && readers + writers > 2)
{
    if (readers > 1) readers--;
    if (writers > 1 && loops * (readers + writers) > 480) writers--;
}

var threads = new Thread[loops];
for (int i = 0; i < loops; i++)
{
    var loop = new Loop(ports, db, readers, writers);
    threads[i] = new Thread(loop.Run) { Name = "loop " + i };
}
foreach (Thread t in threads) t.Start();
Console.WriteLine($"listening on {ports[0]} and {ports[1]} with {loops} loops");
foreach (Thread t in threads) t.Join();

static int Port(string name, int fallback) => int.TryParse(Environment.GetEnvironmentVariable(name), out int p) ? p : fallback;
