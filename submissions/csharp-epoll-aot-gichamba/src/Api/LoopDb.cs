using System.Net.Sockets;
using Feed.Http;
using Feed.Pg;

namespace Feed.Api;

/// <summary>Sends a request's query to Postgres. A loop's pipes in production; a fake in tests.</summary>
public interface IQuerySender {
    /// <summary>Queues <paramref name="query"/> on an open connection and returns at once. False when none is open.</summary>
    bool TrySend(Query query);
}

/// <summary>A statement every connection prepares: its name, SQL and parameter type OIDs.</summary>
public sealed record Statement(string Name, string Sql, uint[] ParamOids);

/// <summary>
/// Picks a pipe for each query. Reads fill one pipe up to <c>pack</c> per pass, so a pass's reads share few Syncs and
/// sends; then the least loaded open pipe takes over. Writes go to the least loaded open pipe.
/// </summary>
public sealed class PipeSender : IQuerySender {
    /// <summary>The pipes to choose from.</summary>
    private readonly PgPipe[] _pipes;
    /// <summary>Most reads put on one pipe in a pass before another is chosen; 0 for writes.</summary>
    private readonly int _pack;
    /// <summary>The pipe this pass's reads are filling.</summary>
    private PgPipe? _current;

    /// <summary>Spreads queries over <paramref name="pipes"/>; <paramref name="pack"/> is 0 for write pipes.</summary>
    public PipeSender(PgPipe[] pipes, int pack) {
        _pipes = pipes;
        _pack = pack;
    }

    /// <inheritdoc />
    public bool TrySend(Query query) {
        PgPipe? p = _current;
        if (p is null || !p.IsOpen || p.Unsynced >= _pack) {
            p = null;
            foreach (PgPipe c in _pipes) {
                if (c.IsOpen && (p is null || c.Load < p.Load)) {
                    p = c;
                }
            }

            if (p is null) {
                return false;
            }

            _current = _pack > 0 ? p : null;
        }

        p.Enqueue(query);
        return true;
    }

    /// <summary>End of a loop pass: every pipe closes its batch and sends.</summary>
    public void EndPass() {
        _current = null;
        foreach (PgPipe p in _pipes) {
            p.EndPass();
        }
    }
}

/// <summary>
/// The Postgres side of one HTTP loop: its read and write pipes, and one thread per pipe that logs in, prepares the
/// statements and hands the socket to the loop. When a pipe loses its connection, its thread connects again after a
/// pause. The loop takes no clients until every pipe has opened, for at most <see cref="StartHoldMs"/>. If Postgres is
/// not up by then, queries on a loop with no open pipe fail at once.
/// </summary>
public sealed class LoopDb {
    /// <summary>Pause in milliseconds before a failed or lost connection is opened again.</summary>
    private const int RetryMs = 1000;
    /// <summary>Longest wait in milliseconds for every pipe to open before the loop takes clients.</summary>
    private const int StartHoldMs = 10_000;

    /// <summary>The owning loop.</summary>
    private readonly HttpLoop _loop;
    /// <summary>Where and how to connect.</summary>
    private readonly PgConnectionString _cs;
    /// <summary>Counts connection failures.</summary>
    private readonly Stats _stats;
    /// <summary>Every pipe with the statements its connection prepares and the signal its loss raises.</summary>
    private readonly List<(PgPipe Pipe, Statement[] Statements, SemaphoreSlim Lost)> _pipes = [];
    /// <summary>Pipes not yet opened since start. The connector that opens the last one resumes accepting.</summary>
    private int _unopened;

    /// <summary>Creates <paramref name="reads"/> read and <paramref name="writes"/> write pipes for <paramref name="loop"/> and hooks them to its passes.</summary>
    public LoopDb(HttpLoop loop, PgConnectionString cs, int reads, int writes, int pack, Stats stats) {
        _loop = loop;
        _cs = cs;
        _stats = stats;
        PipeSender rs = new(NewPipes(reads, false, Sql.Reads), pack), ws = new(NewPipes(writes, true, Sql.Writes), 0);
        Reads = rs;
        Writes = ws;
        loop.OnPassEnd(rs.EndPass);
        loop.OnPassEnd(ws.EndPass);
    }

    /// <summary>Sender for feed, post and health queries.</summary>
    public IQuerySender Reads { get; }
    /// <summary>Sender for inserts.</summary>
    public IQuerySender Writes { get; }

    /// <summary>Holds the loop's accepts and starts one connector thread per pipe. Call it before the loop starts.</summary>
    public void Start() {
        // A client let in before its loop's pipes open gets 503 or 500, while other loops already answer 200.
        _unopened = _pipes.Count;
        _loop.HoldAccepts(StartHoldMs);
        for (int i = 0; i < _pipes.Count; i++) {
            (PgPipe pipe, Statement[] statements, SemaphoreSlim lost) = _pipes[i];
            int slot = i;
            new Thread(() => Connect(pipe, statements, lost, slot)) { IsBackground = true, Name = "pg-connect-" + slot }.Start();
        }
    }

    /// <summary>Creates <paramref name="count"/> pipes whose losses wake their connector threads.</summary>
    private PgPipe[] NewPipes(int count, bool writes, Statement[] statements) {
        PgPipe[] pipes = new PgPipe[count];
        for (int i = 0; i < count; i++) {
            SemaphoreSlim lost = new(0);
            pipes[i] = new PgPipe(_loop, writes, _stats, _ => lost.Release());
            _pipes.Add((pipes[i], statements, lost));
        }

        return pipes;
    }

    /// <summary>The loop of one connector thread: connect, prepare, hand over, wait for the loss, pause, repeat.</summary>
    private void Connect(PgPipe pipe, Statement[] statements, SemaphoreSlim lost, int slot) {
        bool opened = false;
        while (true) {
            PgConnection? conn = null;
            try {
                conn = PgConnection.Connect(_cs, "feed", Sql.Session);
                foreach (Statement s in statements) {
                    conn.Prepare(s.Name, s.Sql, s.ParamOids);
                }

                Socket socket = conn.Detach();
                _loop.Run(() => pipe.Attach(socket));
            } catch (Exception ex) {
                _stats.RecordPgError($"connection {slot}: {ex.Message}");
                conn?.Dispose();
                Thread.Sleep(RetryMs);
                continue;
            }

            if (!opened) {
                opened = true;
                if (Interlocked.Decrement(ref _unopened) == 0) {
                    _loop.ResumeAccepts();
                }
            }

            lost.Wait();
            Thread.Sleep(RetryMs);
        }
    }
}
