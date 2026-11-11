using System;
using System.Text;
using System.Threading;

// Each epoll loop has its own listeners and Postgres connections.
Loop.StartedAt = Environment.TickCount64;
Loop.Jwt = new Jwt(Encoding.UTF8.GetBytes(Environment.GetEnvironmentVariable("JWT_SECRET") ?? "million-users-challenge"));
Loop.Conns = new Conn[Native.MaxFiles()];

var db = PgSettings.Parse(Environment.GetEnvironmentVariable("DATABASE_URL") ?? "postgres://postgres@localhost:5432/feed");
int[] ports = [Port("PORT", 3000), Port("EXTRA_PORT", 3001)];
// On four or more vCPUs, one fewer loop gives the kernel's network processing more scheduling room.
// On the 4-vCPU single-queue rig, three loops reduced p95 and total CPU at the same request rate.
int cores = Environment.ProcessorCount;
int loops = cores >= 4 ? cores - 1 : cores;
// a pass's reads go out as one batch on one reader, so a loop needs only as many readers as batches it can have out
// at once (a backend is one process, one core, and the database has 16): about 32 in all, 2 to 8 a loop. A pass's
// writes go as one group commit on a writer with none out: 2 to 4 a loop. That makes 36 connections on 4
// vCPUs, 42 on 8 and 60 on 16, far under Postgres's 512
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
