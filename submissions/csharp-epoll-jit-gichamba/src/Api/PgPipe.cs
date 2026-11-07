using System.Buffers.Binary;
using System.Net.Sockets;
using Feed.Http;
using Feed.Pg;

namespace Feed.Api;

/// <summary>
/// One Postgres connection driven by an event loop: a non-blocking socket in the loop's epoll set, its statements
/// pipelined, its results parsed and its requests answered on the loop thread. Nothing here is thread-safe; every
/// call comes from the owning loop.
///
/// A read pipe packs the reads of one loop pass behind one Sync and answers each read at its own CommandComplete. A
/// write pipe runs one group at a time: the writes queued while a group commits go out together as the next one, and
/// each is answered when its group's Sync result says it committed.
/// </summary>
public sealed unsafe class PgPipe : IFdHandler {
    /// <summary>Size of the receive buffer; it grows for a larger message.</summary>
    private const int RecvBytes = 64 * 1024;

    /// <summary>The loop that owns this pipe.</summary>
    private readonly HttpLoop _loop;
    /// <summary>True for a write pipe (group commit), false for a read pipe (packs).</summary>
    private readonly bool _writes;
    /// <summary>Counts losses.</summary>
    private readonly Stats _stats;
    /// <summary>Called on the loop thread once the pipe has failed and released its socket.</summary>
    private readonly Action<PgPipe> _lost;
    /// <summary>Encoded messages not yet sent; <see cref="_sentBytes"/> of them already went out.</summary>
    private readonly PgWriter _out = new();
    /// <summary>Bytes of <see cref="_out"/> already sent.</summary>
    private int _sentBytes;
    /// <summary>Received bytes; the unparsed ones lie between <see cref="_inStart"/> and <see cref="_inEnd"/>.</summary>
    private byte[] _in = new byte[RecvBytes];
    /// <summary>Start and end of the unparsed received bytes.</summary>
    private int _inStart, _inEnd;
    /// <summary>Column offsets reused for every DataRow.</summary>
    private int[] _scratch = new int[32];
    /// <summary>Statements sent and not yet answered, oldest first; the server answers in this order.</summary>
    private readonly Queue<Query> _sent = new();
    /// <summary>For every Sync sent and not yet answered, how many statements it closes.</summary>
    private readonly Queue<int> _syncs = new();
    /// <summary>Statements encoded since the last Sync.</summary>
    private int _unsynced;
    /// <summary>Statements of the oldest unanswered Sync that have not had their result yet, once it has started.</summary>
    private int _left;
    /// <summary>A result of the oldest unanswered Sync has arrived, so <see cref="_left"/> counts for it.</summary>
    private bool _started;
    /// <summary>A statement of the oldest Sync failed: Postgres skips the rest until the Sync.</summary>
    private bool _aborted;
    /// <summary>Write pipe: writes waiting for the group in flight to finish.</summary>
    private readonly List<Query> _staged = [];
    /// <summary>Write pipe: members of the group in flight whose results are in, waiting for its Sync.</summary>
    private readonly List<Query> _done = [];
    /// <summary>The socket; kept so the runtime does not close it. Null once released.</summary>
    private Socket? _socket;
    /// <summary>The socket's descriptor, or -1.</summary>
    private int _fd = -1;

    /// <summary>Creates a pipe with no connection yet; <see cref="Attach"/> gives it one.</summary>
    public PgPipe(HttpLoop loop, bool writes, Stats stats, Action<PgPipe> lost) {
        _loop = loop;
        _writes = writes;
        _stats = stats;
        _lost = lost;
    }

    /// <summary>True while the pipe has a working connection.</summary>
    public bool IsOpen => _fd >= 0;

    /// <summary>Statements given to this pipe and not yet answered, a measure of its load. Encoded reads are in <see cref="_sent"/> already.</summary>
    public int Load => _sent.Count + _staged.Count;

    /// <summary>Statements encoded on a read pipe since its last Sync.</summary>
    public int Unsynced => _unsynced;

    /// <summary>
    /// Takes over a logged-in, prepared socket and starts watching it. Call it on the loop thread. On failure the socket
    /// is closed and the pipe stays closed.
    /// </summary>
    public void Attach(Socket socket) {
        socket.Blocking = false;
        _socket = socket;
        _fd = (int)socket.Handle;
        try {
            _loop.Watch(_fd, this);
        } catch (Exception ex) {
            Lose("watch: " + ex.Message);
        }
    }

    /// <summary>Takes a query. A read is encoded at once; a write waits for the group to send it. Call <see cref="EndPass"/> after.</summary>
    public void Enqueue(Query q) {
        if (_writes) {
            _staged.Add(q);
        } else {
            Encode(q);
            _unsynced++;
        }
    }

    /// <summary>
    /// End of a loop pass: closes the pass's pack of reads with one Sync, or sends the staged writes as a group when no
    /// group is in flight, then sends what is pending.
    /// </summary>
    public void EndPass() {
        if (_fd < 0) {
            return;
        }

        if (_writes) {
            if (_syncs.Count == 0 && _staged.Count > 0) {
                SendGroup();
            }
        } else if (_unsynced > 0) {
            CloseBatch();
        }

        Flush();
    }

    /// <inheritdoc />
    public void OnEvent(uint flags) {
        if (_fd < 0) {
            return;
        }

        if ((flags & (Libc.EPOLLIN | Libc.EPOLLRDHUP | Libc.EPOLLHUP | Libc.EPOLLERR)) != 0) {
            Receive();
        }

        if (_fd >= 0 && (flags & Libc.EPOLLOUT) != 0) {
            Flush();
        }

        if (_fd >= 0 && (flags & (Libc.EPOLLHUP | Libc.EPOLLERR)) != 0) {
            Lose("connection closed by server");
        }
    }

    /// <summary>Fails every query this pipe holds, closes the socket and reports the loss. Safe to call twice.</summary>
    public void Lose(string reason) {
        if (_socket is null) {
            return;
        }

        if (_fd >= 0) {
            _loop.Unwatch(_fd);
        }

        _fd = -1;
        try {
            _socket.Dispose();
        } catch (Exception) {
            // Already gone.
        }

        _socket = null;
        _stats.RecordPgError((_writes ? "write" : "read") + " connection lost: " + reason);
        List<Query> lost = [.. _sent, .. _done, .. _staged];
        _sent.Clear();
        _done.Clear();
        _staged.Clear();
        _syncs.Clear();
        _unsynced = _left = 0;
        _aborted = _started = false;
        _out.Clear();
        _sentBytes = 0;
        _inStart = _inEnd = 0;
        foreach (Query q in lost) {
            q.Fail("database connection lost");
        }

        _lost(this);
    }

    /// <summary>Writes Bind and Execute for <paramref name="q"/> and tracks it as sent.</summary>
    private void Encode(Query q) {
        PgEncoder.Bind(_out, q.StatementName, q.Params);
        PgEncoder.Execute(_out);
        _sent.Enqueue(q);
    }

    /// <summary>Ends the statements encoded since the last Sync with a Sync of their own.</summary>
    private void CloseBatch() {
        PgEncoder.Sync(_out);
        _syncs.Enqueue(_unsynced);
        _unsynced = 0;
    }

    /// <summary>
    /// Sends the staged writes as one group, one Sync and one commit. A write retried after a failed group goes alone,
    /// so its answer is its own.
    /// </summary>
    private void SendGroup() {
        int n = _staged[0].RetryAlone ? 1 : _staged.Count;
        for (int i = 0; i < n; i++) {
            Encode(_staged[i]);
        }

        _staged.RemoveRange(0, n);
        _unsynced = n;
        CloseBatch();
    }

    /// <summary>Sends pending bytes until done or the socket is full; EPOLLOUT resumes it.</summary>
    private void Flush() {
        int len = _out.Length;
        if (_sentBytes == len) {
            return;
        }

        fixed (byte* p = _out.Written) {
            while (_sentBytes < len) {
                nint n = Libc.send(_fd, p + _sentBytes, (nuint)(len - _sentBytes), Libc.MSG_NOSIGNAL);
                if (n > 0) {
                    _sentBytes += (int)n;
                    continue;
                }

                int err = Libc.Errno;
                if (err == Libc.EINTR) {
                    continue;
                }

                if (err == Libc.EAGAIN) {
                    return;
                }

                Lose($"send failed, errno {err}");
                return;
            }
        }

        _out.Clear();
        _sentBytes = 0;
    }

    /// <summary>Reads until the socket is drained, handling each whole message as it arrives.</summary>
    private void Receive() {
        while (_fd >= 0) {
            if (_inStart > 0 && _inStart == _inEnd) {
                _inStart = _inEnd = 0;
            }

            if (_inEnd == _in.Length) {
                Compact();
            }

            nint n;
            fixed (byte* p = _in) {
                n = Libc.recv(_fd, p + _inEnd, (nuint)(_in.Length - _inEnd), 0);
            }

            if (n > 0) {
                _inEnd += (int)n;
                Parse();
                continue;
            }

            if (n == 0) {
                Lose("connection closed by server");
                return;
            }

            int err = Libc.Errno;
            if (err == Libc.EINTR) {
                continue;
            }

            if (err != Libc.EAGAIN) {
                Lose($"recv failed, errno {err}");
            }

            return;
        }
    }

    /// <summary>Moves the unparsed bytes to the front, growing the buffer when one message needs more room.</summary>
    private void Compact() {
        int avail = _inEnd - _inStart;
        int need = avail;
        if (avail >= 5) {
            need = Math.Max(need, BinaryPrimitives.ReadInt32BigEndian(_in.AsSpan(_inStart + 1)) + 1);
        }

        byte[] target = need > _in.Length ? new byte[Math.Max(need, _in.Length * 2)] : _in;
        Buffer.BlockCopy(_in, _inStart, target, 0, avail);
        _in = target;
        _inStart = 0;
        _inEnd = avail;
    }

    /// <summary>Handles every whole message in the buffer.</summary>
    private void Parse() {
        while (_fd >= 0 && _inEnd - _inStart >= 5) {
            int len = BinaryPrimitives.ReadInt32BigEndian(_in.AsSpan(_inStart + 1));
            if (len < 4) {
                Lose("bad message length " + len);
                return;
            }

            if (_inEnd - _inStart < len + 1) {
                return;
            }

            byte type = _in[_inStart];
            ReadOnlySpan<byte> body = _in.AsSpan(_inStart + 5, len - 4);
            _inStart += len + 1;
            OnMessage(type, body);
        }
    }

    /// <summary>Applies one backend message to the oldest statement or Sync in flight.</summary>
    private void OnMessage(byte type, ReadOnlySpan<byte> body) {
        switch ((char)type) {
            case 'D':
                Head()?.OnRow(PgRow.Parse(body, ref _scratch));
                break;
            case 'C':
                if (Head() is { } done) {
                    done.OnCommandComplete(PgMessages.ParseTag(body));
                    Finish(_sent.Dequeue());
                }

                break;
            case 'E':
                if (Head() is { } failed) {
                    failed.OnError(PgMessages.ParseError(body));
                    _aborted = true;
                    Finish(_sent.Dequeue());
                } else {
                    Lose("error with nothing in flight: " + PgMessages.ParseError(body).Message);
                }

                break;
            case 'Z':
                EndSync();
                break;
            default:
                // BindComplete, NoData, notices and parameter changes carry nothing a request needs.
                break;
        }
    }

    /// <summary>The statement the next result belongs to, or null after an error skipped the rest of its Sync.</summary>
    private Query? Head() {
        if (!_started) {
            if (_syncs.Count == 0) {
                return null;
            }

            _left = _syncs.Peek();
            _started = true;
        }

        return _aborted || _left == 0 ? null : _sent.Peek();
    }

    /// <summary>A statement has its result or error. A read is answered now; a write waits for its group's Sync.</summary>
    private void Finish(Query q) {
        _left--;
        if (_writes) {
            _done.Add(q);
        } else {
            q.OnDone();
        }
    }

    /// <summary>
    /// ReadyForQuery: the oldest Sync is answered. Reads it skipped after an error are sent again. A write group that
    /// committed is answered; one that failed rolled back, so each member is retried on its own.
    /// </summary>
    private void EndSync() {
        if (_syncs.Count == 0) {
            Lose("ReadyForQuery with no Sync in flight");
            return;
        }

        int size = _syncs.Dequeue();
        int left = _started ? _left : size;
        List<Query>? skipped = null;
        for (; left > 0; left--) {
            (skipped ??= []).Add(_sent.Dequeue());
        }

        bool failed = _aborted;
        _aborted = _started = false;
        _left = 0;
        if (_writes) {
            List<Query> group = [.. _done];
            _done.Clear();
            if (skipped is not null) {
                group.AddRange(skipped);
            }

            if (failed && size > 1) {
                foreach (Query q in group) {
                    q.Reset();
                    q.RetryAlone = true;
                }

                _staged.InsertRange(0, group);
            } else {
                foreach (Query q in group) {
                    q.OnDone();
                }
            }

            if (_staged.Count > 0) {
                SendGroup();
                Flush();
            }
        } else if (skipped is not null) {
            foreach (Query q in skipped) {
                Encode(q);
                _unsynced++;
            }

            CloseBatch();
            Flush();
        }
    }
}
