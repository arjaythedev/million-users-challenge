using System;
using System.Text;
using System.Threading;

// Each epoll loop has its own listeners and Postgres connections.
Loop.StartedAt = Environment.TickCount64;
Loop.Jwt = new Jwt(Encoding.UTF8.GetBytes(Environment.GetEnvironmentVariable("JWT_SECRET") ?? "million-users-challenge"));
Loop.Conns = new Conn[Native.MaxFiles()];

var db = PgSettings.Parse(Environment.GetEnvironmentVariable("DATABASE_URL") ?? "postgres://postgres@localhost:5432/feed");
int[] ports = [Port("PORT", 3000), Port("EXTRA_PORT", 3001)];
// One loop per core. One fewer saved CPU on a CPU-bound test box, but the scoring box runs out of network before CPU,
// and there it held fewer users: 1,360,000 on 4 cores with 3 loops against 1,390,000 with 4 (fsharp-kestrel-uring
// lost the same way, 1,350,000 against 1,510,000).
int loops = Environment.ProcessorCount;
// Most of Postgres's 512 connections, though a quiet network needs few: a pass's reads go out as one batch on one
// reader, and a handful of readers taking turns carry every pass. They are there for when packets are lost. A
// connection that keeps losing packets carries little (TCP at Postgres's end shrinks its window and waits out
// timers), and what it does carry holds up the reads queued behind it, so the reads then go out in more and smaller
// batches, each on a reader of its own, and how many requests a loop can serve is how many readers it has.
// Writers carry one group commit at a time. 496 in all at 4, 8 and 16 vCPUs: 116 readers and 8 writers a loop on 4.
const int Connections = 496;
loops = Math.Min(loops, Connections / 2); // a loop needs a reader and a writer
int perLoop = Connections / loops;
int writers = Math.Clamp(32 / loops, 1, Math.Min(8, perLoop - 1)), readers = perLoop - writers;

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
