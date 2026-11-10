using System;

/// <summary>The routes: each request is answered at once (an error) or queued for Postgres as a read or a
/// write.</summary>
sealed partial class Loop
{
    void Route(int fd, ReadOnlySpan<byte> method, ReadOnlySpan<byte> path, bool hasAuthorization, ReadOnlySpan<byte> authorization, ReadOnlySpan<byte> body)
    {
        bool get = method.SequenceEqual("GET"u8), post = method.SequenceEqual("POST"u8);
        if (get && path.SequenceEqual("/feed"u8))
        {
            QueueRead(fd, Pg.Feed, 0);
            return;
        }
        if (path.StartsWith("/posts"u8))
        {
            ReadOnlySpan<byte> rest = path[6..];
            if (rest.IsEmpty)
            {
                if (post) CreatePost(fd, hasAuthorization, authorization, body);
                else Error(fd, 404, "not found"u8);
                return;
            }
            if (rest[0] == '/')
            {
                rest = rest[1..];
                int slash = rest.IndexOf((byte)'/');
                if (slash < 0 && get)
                {
                    if (Jwt.PositiveInteger(rest, out long id)) QueueRead(fd, Pg.Post, id);
                    else Error(fd, 400, "invalid post id"u8);
                    return;
                }
                if (slash >= 0 && post && rest[(slash + 1)..].SequenceEqual("like"u8))
                {
                    LikePost(fd, hasAuthorization, authorization, rest[..slash]);
                    return;
                }
            }
        }
        else if (get && path.SequenceEqual("/health"u8))
        {
            QueueRead(fd, Pg.Health, 0);
            return;
        }
        Error(fd, 404, "not found"u8);
    }

    /// <summary>The user behind the request's token, or false after answering 401.</summary>
    bool Authorize(int fd, bool hasAuthorization, ReadOnlySpan<byte> authorization, out long user, out int nameLength)
    {
        user = 0;
        nameLength = 0;
        TokenCheck check = hasAuthorization ? Jwt.Check(authorization, _userName, out user, out nameLength) : TokenCheck.Missing;
        switch (check)
        {
            case TokenCheck.Valid: return true;
            case TokenCheck.Missing: Error(fd, 401, "missing bearer token"u8); break;
            case TokenCheck.Invalid: Error(fd, 401, "invalid or expired token"u8); break;
            default: Error(fd, 401, "invalid token payload"u8); break;
        }
        return false;
    }

    void CreatePost(int fd, bool hasAuthorization, ReadOnlySpan<byte> authorization, ReadOnlySpan<byte> body)
    {
        if (!Authorize(fd, hasAuthorization, authorization, out long user, out int nameLength)) return;
        switch (Json.Body(body, _postBody, out int length))
        {
            case BodyCheck.Malformed: Error(fd, 400, "malformed JSON body"u8); return;
            case BodyCheck.Required: Error(fd, 400, "body is required"u8); return;
            case BodyCheck.TooLong: Error(fd, 400, "body must be at most 500 characters"u8); return;
        }
        WriteJob job = NewWrite(fd, WriteJob.Create, user);
        _postBody.AsSpan(0, length).CopyTo(job.Body);
        job.BodyLength = length;
        _userName.AsSpan(0, nameLength).CopyTo(job.Name);
        job.NameLength = nameLength;
        _writes.Add(job);
    }

    void LikePost(int fd, bool hasAuthorization, ReadOnlySpan<byte> authorization, ReadOnlySpan<byte> id)
    {
        if (!Authorize(fd, hasAuthorization, authorization, out long user, out _)) return;
        if (!Jwt.PositiveInteger(id, out long postId))
        {
            Error(fd, 400, "invalid post id"u8);
            return;
        }
        WriteJob job = NewWrite(fd, WriteJob.Like, user);
        job.PostId = postId;
        _writes.Add(job);
    }

    /// <summary>A write job for the request, reused from <see cref="_spareJobs"/> when one is there; the connection
    /// waits on it from now on.</summary>
    WriteJob NewWrite(int fd, byte kind, long user)
    {
        WriteJob job = _spareJobs.Count > 0 ? _spareJobs.Pop() : new WriteJob();
        job.Fd = fd;
        job.Gen = Conns[fd].Gen;
        job.Kind = kind;
        job.UserId = user;
        Conns[fd].Busy = true;
        return job;
    }
}
