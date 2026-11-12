using System;
using System.Buffers.Binary;
using System.Collections.Generic;

/// <summary>The loop's Postgres side: each pass's reads sent as one batch on a reader and its writes as one group on a
/// writer, replies turned into responses, and connections that drop or reconnect. Which reader takes a batch and how
/// many reads a batch holds follow how the network is doing (<see cref="NextReader"/>, <see cref="Judge"/>).</summary>
sealed partial class Loop
{
    // ---- reads

    /// <summary>Queues a read in this pass's batch. A pass's reads go to one reader, under one Sync, so they cost one
    /// packet each way and one wake-up of one Postgres backend; past <see cref="_batch"/> reads the next reader takes
    /// over.</summary>
    void QueueRead(int fd, byte statement, long id)
    {
        Pg? pg = _passReader;
        if (pg == null || pg.Broken || pg.Open >= _batch)
        {
            if (pg != null && !pg.Broken) pg.Seal(_now);
            pg = _passReader = NextReader(_pgs, _readers, _now, _turns);
            if (pg == null)
            {
                if (statement == Pg.Health) Unhealthy(fd, "database connection is down"u8);
                else Error(fd, 500, "internal server error"u8);
                return;
            }
        }
        pg.Read(statement, id, fd, Conns[fd].Gen);
        Conns[fd].Busy = true;
    }

    /// <summary>The reader a new batch goes to. A free one, so the batch waits behind nothing: the first
    /// <paramref name="turns"/> readers take turns (the one whose last batch is oldest), and when all of those are
    /// busy, the first free one past them. When none is free, the one with the fewest reads waiting among those
    /// still answering; a reader that has gone silent on what it owes (<see cref="Pg.Stalled"/>) is taken only when
    /// all of them have, since reads queued behind a lost packet wait for it however few they are. Null when every
    /// reader is down.
    /// <para>Turns, not always the first free reader: with packets dropped on purpose, about a fifth fewer reads
    /// stalled that way. A connection that has just lost packets has a small window at Postgres's end; taking
    /// turns, it isn't the one every next batch goes to.</para></summary>
    internal static Pg? NextReader(Pg[] pgs, int readers, long now, int turns)
    {
        Pg? best = null;
        turns = Math.Min(turns, readers);
        for (int i = 0; i < turns; i++)
        {
            Pg c = pgs[i];
            if (!c.Broken && c.Pending == 0 && (best == null || c.LastUsed < best.LastUsed)) best = c;
        }
        for (int i = turns; i < readers && best == null; i++)
            if (!pgs[i].Broken && pgs[i].Pending == 0) best = pgs[i];
        if (best != null) return best;

        bool bestStalled = true;
        for (int i = 0; i < readers; i++)
        {
            Pg c = pgs[i];
            if (c.Broken) continue;
            bool stalled = c.Stalled(now);
            if (best == null || (bestStalled && !stalled) || (bestStalled == stalled && c.Pending < best.Pending))
            {
                best = c;
                bestStalled = stalled;
            }
        }
        return best;
    }

    // ---- how many reads a batch holds, and how many readers take turns

    /// <summary>The least a batch shrinks to (<see cref="Judge"/>).</summary>
    internal const int SmallestBatch = 4;
    /// <summary>The fewest readers that take turns.</summary>
    internal const int FewestTurns = 8;
    /// <summary>A read that waited this long for Postgres waited through more than one retransmit timer.</summary>
    internal const int LongWaitMs = 350;
    static readonly long LongWait = LongWaitMs * System.Diagnostics.Stopwatch.Frequency / 1000;
    static readonly long JudgeEvery = System.Diagnostics.Stopwatch.Frequency / 4;
    /// <summary>The fewest reads a judgment is made on.</summary>
    const int JudgeReads = 100;

    /// <summary>The most reads a batch holds now, <see cref="SmallestBatch"/> to <see cref="Pg.MaxBatch"/>. A batch's
    /// rows come back as one stream, so a lost packet holds up every read behind it, and the longer the stream, the
    /// likelier one of its packets is lost twice. While reads wait through retransmit timers, batches are halved:
    /// more of them, on more readers, each less exposed. Once they stop, batches grow back, because fewer and
    /// fuller packets are what a healthy network wants; and they grow whenever nearly every reader is busy, because
    /// a small batch behind another on a busy reader is worse off than a large one on its own.</summary>
    int _batch = Pg.MaxBatch;
    /// <summary>How many readers take turns: twice as many as were busy lately, <see cref="FewestTurns"/> at least.
    /// On a quiet network a few carry every pass and Postgres keeps as few backends warm; under loss, batches take
    /// longer, more are out at once, and the turns widen with them.</summary>
    int _turns = FewestTurns;
    long _judgedAt;
    int _judged, _late, _busyPeak;

    /// <summary>Every quarter of a second: halves or doubles the batch size by the share of reads that waited
    /// <see cref="LongWaitMs"/> or more, those still waiting included, and sets how many readers take turns.</summary>
    void Judge()
    {
        if (_now - _judgedAt < JudgeEvery) return;
        _judgedAt = _now;
        int busy = 0;
        for (int i = 0; i < _readers; i++)
        {
            Pg c = _pgs[i];
            if (c.Broken || c.Pending == 0) continue;
            busy++;
            _judged += c.Pending;
            if (c.SilentFor(_now) >= LongWait) _late += c.Pending;
        }
        _busyPeak = Math.Max(busy, _busyPeak * 3 / 4);
        _turns = Math.Max(FewestTurns, 2 * _busyPeak + 4);
        if (_judged < JudgeReads) return;
        _batch = NextBatch(_batch, _judged, _late, busy, _readers);
        _judged = _late = 0;
    }

    /// <summary>The batch size after a judgment: double when nine readers in ten are busy (there is no reader left
    /// for more batches) or fewer than 0.25 % of the reads waited long, half when more than 1 % did, within
    /// <see cref="SmallestBatch"/> and <see cref="Pg.MaxBatch"/>.</summary>
    internal static int NextBatch(int batch, int reads, int late, int busy, int readers)
    {
        if (busy * 10L >= readers * 9L || late * 400L < reads) return Math.Min(batch * 2, Pg.MaxBatch);
        if (late * 100L > reads) return Math.Max(batch / 2, SmallestBatch);
        return batch;
    }

    /// <summary>A read failed inside a batch: Postgres skipped the reads after it up to the Sync, so they go again,
    /// with this pass's reads.</summary>
    void RetryRestOfBatch(Pg pg, Waiter failed)
    {
        if (failed.Last) return;
        while (pg.Pending > 0)
        {
            Waiter w = pg.TakeWaiter();
            if (Conns[w.Fd].Gen == w.Gen)
            {
                Conns[w.Fd].Busy = false;
                QueueRead(w.Fd, w.Kind, w.Id);
                Resume(w.Fd, w.Gen);
            }
            if (w.Last) return;
        }
    }

    /// <summary>Answers a read from its result: the rows written as JSON as they are walked, or the error.</summary>
    void OnReadReply(Waiter w, ReadOnlySpan<byte> reply)
    {
        int fd = w.Fd;
        int at = HeadRoom;
        byte[] b = _response;
        scoped ReadOnlySpan<byte> error = default;
        bool failed = false, found = false;
        if (w.Kind == Pg.Feed) Json.Raw(b, ref at, "{\"posts\":["u8);
        else if (w.Kind == Pg.Post) Json.Raw(b, ref at, "{\"post\":"u8);
        while (Pg.NextMessage(ref reply, out byte type, out ReadOnlySpan<byte> m))
        {
            if (type == 'D')
            {
                if (w.Kind == Pg.Health) continue;
                if (found && w.Kind == Pg.Feed) b[at++] = (byte)',';
                found = true;
                Json.Post(b, ref at, m);
            }
            else if (type == 'E')
            {
                failed = true;
                error = m;
            }
        }
        if (failed)
        {
            if (w.Kind == Pg.Health) Unhealthy(fd, Pg.ErrorField(error, (byte)'M'));
            else Error(fd, 500, "internal server error"u8);
        }
        else if (w.Kind == Pg.Health)
        {
            at = HeadRoom;
            Json.Raw(b, ref at, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"u8);
            Json.Long(b, ref at, (Environment.TickCount64 - StartedAt) / 1000);
            b[at++] = (byte)'}';
            Respond(fd, 200, at);
        }
        else if (w.Kind == Pg.Feed)
        {
            Json.Raw(b, ref at, "]}"u8);
            Respond(fd, 200, at);
        }
        else if (found)
        {
            b[at++] = (byte)'}';
            Respond(fd, 200, at);
        }
        else Error(fd, 404, "post not found"u8);
    }

    // ---- writes

    /// <summary>Sends the waiting writes as one group (one transaction, one commit) on a writer that has none out.
    /// While every writer waits for its commit they stay queued and leave with the pass that finds one free, so a
    /// slow or lost commit holds up its own group, not the writes behind it.</summary>
    void StartWriteGroups()
    {
        while (_writes.Count > 0)
        {
            Pg? writer = null;
            bool alive = false;
            for (int i = _readers; i < _pgs.Length && writer == null; i++)
            {
                Pg c = _pgs[i];
                if (c.Broken) continue;
                alive = true;
                if (c.Group.Count == 0) writer = c;
            }
            if (writer == null)
            {
                if (alive) return;
                foreach (WriteJob job in _writes) FailWrite(job);
                _writes.Clear();
                return;
            }
            int n = Math.Min(_writes.Count, MaxGroup);
            for (int i = 0; i < n; i++) writer.Group.Add(_writes[i]);
            _writes.RemoveRange(0, n);
            writer.SendGroup(_now);
        }
    }

    /// <summary>The group's reply: each write's outcome in the order sent, or the error that rolled them all
    /// back.</summary>
    void OnWriteGroupReply(Pg writer, ReadOnlySpan<byte> reply)
    {
        List<WriteJob> group = writer.Group;
        int k = 0, failedAt = -1;
        scoped ReadOnlySpan<byte> error = default;
        while (Pg.NextMessage(ref reply, out byte type, out ReadOnlySpan<byte> m))
        {
            if (type == 'D' && k < group.Count)
            {
                // RETURNING id, created_at: the column count, then each column's length (4) and its int8 (8)
                ReadOnlySpan<byte> row = m[2..];
                group[k].NewId = BinaryPrimitives.ReadInt64BigEndian(row[4..]);
                group[k].CreatedAt = BinaryPrimitives.ReadInt64BigEndian(row[(4 + 8 + 4)..]);
            }
            else if (type == 'C' && k < group.Count)
            {
                // "INSERT 0 1\0": the row count is the tag's last digit before the terminator
                group[k].Inserted = m.Length >= 2 && m[^2] == '1' ? 1 : 0;
                k++;
            }
            else if (type == 'E')
            {
                failedAt = k;
                error = m;
                break;
            }
        }

        if (failedAt < 0)
        {
            foreach (WriteJob job in group) AnswerWrite(job);
        }
        else if (failedAt >= group.Count)
        {
            foreach (WriteJob job in group) FailWrite(job);
        }
        else
        {
            // the transaction rolled back: the failing write gets its error, the others go again
            WriteJob bad = group[failedAt];
            // 23503 is foreign_key_violation: a like on a post that doesn't exist
            if (bad.Kind == WriteJob.Like && Pg.ErrorField(error, (byte)'C').SequenceEqual("23503"u8))
            {
                if (Conns[bad.Fd].Gen == bad.Gen)
                {
                    Conns[bad.Fd].Busy = false;
                    Error(bad.Fd, 404, "post not found"u8);
                    Resume(bad.Fd, bad.Gen);
                }
                _spareJobs.Push(bad);
            }
            else FailWrite(bad);
            group.RemoveAt(failedAt);
            _writes.InsertRange(0, group);
        }
        group.Clear();
    }

    /// <summary>Answers a committed write (201 for a new post or like, 200 for a like already there) and spares its
    /// job.</summary>
    void AnswerWrite(WriteJob job)
    {
        int fd = job.Fd;
        if (Conns[fd].Gen == job.Gen)
        {
            Conns[fd].Busy = false;
            byte[] b = _response;
            int at = HeadRoom;
            int status;
            if (job.Kind == WriteJob.Create)
            {
                Json.Raw(b, ref at, "{\"post\":{\"id\":"u8);
                Json.Long(b, ref at, job.NewId);
                Json.Raw(b, ref at, ",\"body\":"u8);
                Json.String(b, ref at, job.Body.AsSpan(0, job.BodyLength));
                Json.Raw(b, ref at, ",\"created_at\":"u8);
                Json.Timestamp(b, ref at, job.CreatedAt);
                Json.Raw(b, ref at, ",\"author\":"u8);
                Json.String(b, ref at, job.Name.AsSpan(0, job.NameLength));
                Json.Raw(b, ref at, ",\"like_count\":0}}"u8);
                status = 201;
            }
            else
            {
                Json.Raw(b, ref at, job.Inserted == 1 ? "{\"liked\":true,\"already_liked\":false,\"post_id\":"u8 : "{\"liked\":true,\"already_liked\":true,\"post_id\":"u8);
                Json.Long(b, ref at, job.PostId);
                b[at++] = (byte)'}';
                status = job.Inserted == 1 ? 201 : 200;
            }
            Respond(fd, status, at);
            Resume(fd, job.Gen);
        }
        _spareJobs.Push(job);
    }

    /// <summary>Answers a write that can't be done with 500 and spares its job.</summary>
    void FailWrite(WriteJob job)
    {
        if (Conns[job.Fd].Gen == job.Gen)
        {
            Conns[job.Fd].Busy = false;
            Error(job.Fd, 500, "internal server error"u8);
            Resume(job.Fd, job.Gen);
        }
        _spareJobs.Push(job);
    }

    // ---- connections

    void OnPg(int i, uint events)
    {
        Pg pg = _pgs[i];
        if (pg.Connecting)
        {
            OnPgConnected(i);
            return;
        }
        if (pg.Broken) return;
        if ((events & (Native.EpollErr | Native.EpollHup)) != 0)
        {
            OnPgLost(i);
            return;
        }
        if ((events & Native.EpollOut) != 0) FlushPg(i);
        if ((events & Native.EpollIn) == 0) return;
        bool more = true;
        while (more && !pg.Broken)
        {
            if (!pg.Receive(_now, out more))
            {
                OnPgLost(i);
                return;
            }
            if (i >= _readers)
            {
                while (!pg.Broken && pg.NextReply(out ReadOnlySpan<byte> reply, out bool group))
                    if (group) OnWriteGroupReply(pg, reply);
                continue;
            }
            while (!pg.Broken && pg.NextResult(out ReadOnlySpan<byte> result, out byte end))
            {
                if (end == 'Z' || pg.Pending == 0) continue;
                Waiter w = pg.TakeWaiter();
                _judged++;
                if (_now - w.Sent >= LongWait) _late++;
                if (end == 'E') RetryRestOfBatch(pg, w);
                if (Conns[w.Fd].Gen != w.Gen) continue;
                Conns[w.Fd].Busy = false;
                OnReadReply(w, result);
                Resume(w.Fd, w.Gen);
            }
        }
    }

    /// <summary>Sends what the connection has queued, and watches it for writable only while some is left.</summary>
    void FlushPg(int i)
    {
        Pg pg = _pgs[i];
        if (pg.Broken) return;
        if (!pg.Flush())
        {
            OnPgLost(i);
            return;
        }
        bool wantOut = pg.WantsWrite;
        if (wantOut != _pgWatchingOut[i])
        {
            Native.Watch(_epfd, Native.EpollCtlMod, pg.Fd, Native.EpollIn | (wantOut ? Native.EpollOut : 0), PgTag | (uint)i);
            _pgWatchingOut[i] = wantOut;
        }
    }

    /// <summary>A Postgres connection dropped: whatever it owed fails, and it reconnects within a second.</summary>
    void OnPgLost(int i)
    {
        Pg pg = _pgs[i];
        pg.Close();
        if (i >= _readers)
        {
            foreach (WriteJob job in pg.Group) FailWrite(job);
            pg.Group.Clear();
        }
        while (pg.Pending > 0)
        {
            Waiter w = pg.TakeWaiter();
            if (Conns[w.Fd].Gen != w.Gen) continue;
            Conns[w.Fd].Busy = false;
            if (w.Kind == Pg.Health) Unhealthy(w.Fd, "database connection lost"u8);
            else Error(w.Fd, 500, "internal server error"u8);
            Resume(w.Fd, w.Gen);
        }
    }

    /// <summary>Starts connecting; the connection's first writable event finishes the login in <see cref="OnPgConnected"/>.</summary>
    void Reconnect(int i)
    {
        Pg pg = _pgs[i];
        try
        {
            pg.BeginOpen();
            Native.Watch(_epfd, Native.EpollCtlAdd, pg.Fd, Native.EpollOut, PgTag | (uint)i);
        }
        catch (Exception e)
        {
            Console.Error.WriteLine("postgres: " + e.Message);
            pg.Close();
        }
    }

    void OnPgConnected(int i)
    {
        Pg pg = _pgs[i];
        try
        {
            Native.Watch(_epfd, Native.EpollCtlMod, pg.Fd, Native.EpollIn, PgTag | (uint)i);
            _pgWatchingOut[i] = false;
            pg.FinishOpen();
        }
        catch (Exception e)
        {
            Console.Error.WriteLine("postgres: " + e.Message);
            pg.Close();
        }
    }

    bool AnyBroken()
    {
        foreach (Pg pg in _pgs)
            if (pg.Broken) return true;
        return false;
    }
}
