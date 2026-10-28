using System;
using System.Text;
using System.Threading;

// One epoll loop per core, each with its own listeners and Postgres connections.
Loop.StartedAt = Environment.TickCount64;
Loop.Jwt = new Jwt(Encoding.UTF8.GetBytes(Environment.GetEnvironmentVariable("JWT_SECRET") ?? "million-users-challenge"));
Loop.Conns = new Conn[Native.MaxFiles()];

var db = PgSettings.Parse(Environment.GetEnvironmentVariable("DATABASE_URL") ?? "postgres://postgres@localhost:5432/feed");
int[] ports = [Port("PORT", 3000), Port("EXTRA_PORT", 3001)];
// one loop per core. Fewer cost less per request but have no margin: at 25% over the contest's load on 4 cores,
// 4 loops kept p99 at 853 ms where 3 reached 1,021 ms and 2 pinned both loops at 1.5 s
int loops = Environment.ProcessorCount;
// reads pipeline in batches; about 32 read connections in all keep a many-core database busy however few loops
// there are (a backend is one process, one core). On 4 cores 2, 4 and 8 per loop cost the same at the contest's
// load. Writes go as one group commit per loop
const int ReadConnectionsInAll = 32, MaxReadersPerLoop = 8;
// every loop's readers and its writer stay within 480 connections in all, under Postgres's 512
const int MaxConnections = 480;
int readers = Math.Max(1, Math.Min(Math.Min(MaxReadersPerLoop, (ReadConnectionsInAll + loops - 1) / loops),
    MaxConnections / loops - 1));

var threads = new Thread[loops];
for (int i = 0; i < loops; i++)
{
    var loop = new Loop(ports, db, readers);
    threads[i] = new Thread(loop.Run) { Name = "loop " + i };
}
foreach (Thread t in threads) t.Start();
Console.WriteLine($"listening on {ports[0]} and {ports[1]} with {loops} loops");
foreach (Thread t in threads) t.Join();

static int Port(string name, int fallback) => int.TryParse(Environment.GetEnvironmentVariable(name), out int p) ? p : fallback;
