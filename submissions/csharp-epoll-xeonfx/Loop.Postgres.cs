using System;
using System.Buffers.Binary;
using System.Collections.Generic;

/// <summary>The loop's Postgres side: each pass's reads sent as one batch on a reader and its writes as one group on a
/// writer, replies turned into responses, and connections that drop or reconnect.</summary>
sealed partial class Loop
{
    // ---- reads

    /// <summary>Queues a read in this pass's batch. A pass's reads go to one reader, under one Sync, so they cost one
    /// packet each way and one wake-up of one Postgres backend; past <see cref="Pg.MaxBatch"/> the next reader takes
    /// over.</summary>
    void QueueRead(int fd, byte statement, long id)
    {
        Pg? pg = _passReader;
        if (pg == null || pg.Broken || pg.Open >= Pg.MaxBatch)
        {
            if (pg != null && !pg.Broken) pg.Seal(_now);
            pg = _passReader = LeastLoaded(_pgs, _readers);
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

    /// <summary>The reader with the fewest reads waiting, the first of them: while the first readers keep up they
    /// carry every pass, so their packets follow each other closely (a lost one is noticed by the next) and the
    /// others stay free for a pass that finds them busy. Null when every reader is down.</summary>
    internal static Pg? LeastLoaded(Pg[] pgs, int readers)
    {
        Pg? best = null;
        for (int i = 0; i < readers; i++)
        {
            Pg c = pgs[i];
            if (!c.Broken && (best == null || c.Pending < best.Pending)) best = c;
        }
        return best;
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
