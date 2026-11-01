using System.Buffers;
using System.Globalization;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace MillionReactor;

internal sealed class Worker : IDisposable
{
    private const int AcceptBackoffMilliseconds = 100;
    private static readonly DbResponse HealthBusy = new(503,
        "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"server busy\"}"u8.ToArray());
    private static readonly DbResponse BusyResponse = new(503, "{\"error\":\"server busy\"}"u8.ToArray());
    private readonly IEventPoller poller;
    private readonly PgShard database;
    private readonly byte[] key;
    private readonly int[] listeners;
    private readonly Dictionary<int, Client> clients = new();
    private readonly Queue<Client> buffered = new();
    private readonly Client?[] expiry = new Client[256];
    private long lastSecond = Environment.TickCount64 / 1000;
    private readonly int index;
    private long completed;
    private long acceptedClients, listenerBackoffs, acceptErrorLogs;
    private long acceptRetryAt, nextAcceptErrorLogAt;
    private int maxContinuations;
    public Worker(int index, int count, string url, string host, int[] ports, byte[] key, bool uring)
    {
        this.index = index;
        this.key = key;
        poller = uring ? new UringPoller() : new EpollPoller();
        database = new PgShard(url, index, count,
            (fd, write) => poller.Watch(fd, Linux.Read | (write ? Linux.Write : 0)), poller.Remove);
        listeners = ports.Distinct().Select(p => Linux.Listener(host, p)).ToArray();
        foreach (int fd in listeners) poller.Watch(fd, Linux.Read);
    }
    public void Run(CancellationToken stop)
    {
        Span<PollEvent> events = stackalloc PollEvent[512];
        while (!stop.IsCancellationRequested)
        {
            if (acceptRetryAt != 0 && Environment.TickCount64 >= acceptRetryAt)
            {
                acceptRetryAt = 0;
                foreach (int listener in listeners) poller.Watch(listener, Linux.Read);
            }
            database.Tick();
            for (int i = 0, count = Math.Min(buffered.Count, 256); i < count; i++)
            {
                var client = buffered.Dequeue();
                client.Queued = false;
                if (!client.Closed && !client.Busy) client.Dispatch();
            }
            int ready = poller.Wait(events, buffered.Count > 0 ? 0 : 50);
            for (int i = 0; i < ready; i++)
            {
                var ev = events[i];
                // Earlier callbacks in this batch may close and reuse this fd.
                if (!poller.IsCurrent(ev)) continue;
                if (database.Owns(ev.FileDescriptor)) database.OnReady(ev.FileDescriptor);
                else if (Array.IndexOf(listeners, ev.FileDescriptor) >= 0) Accept(ev.FileDescriptor);
                else if (clients.TryGetValue(ev.FileDescriptor, out var client))
                {
                    if ((ev.Events & (Linux.Error | Linux.Hangup)) != 0) { client.Close(); continue; }
                    if ((ev.Events & Linux.Read) != 0 && !client.Busy) client.Read();
                    if (!client.Closed && (ev.Events & Linux.Write) != 0) client.Flush();
                }
            }
            long second = Environment.TickCount64 / 1000;
            while (lastSecond < second)
            {
                lastSecond++;
                var client = expiry[(int)(lastSecond & 255)];
                while (client is not null)
                {
                    var next = client.Next;
                    if (client.Expires <= lastSecond) client.Close();
                    client = next;
                }
            }
        }
    }
    private void Accept(int listener)
    {
        for (int i = 0; i < 128; i++)
        {
            int fd = Linux.accept4(listener, 0, 0, 2048 | 0x80000);
            if (fd < 0)
            {
                int errno = Marshal.GetLastPInvokeError();
                if (errno == 4) continue;
                long now = Environment.TickCount64;
                if (errno is 23 or 24) // ENFILE / EMFILE: suppress readiness until a bounded retry.
                {
                    acceptRetryAt = now + AcceptBackoffMilliseconds;
                    listenerBackoffs++;
                    // Both listeners share this worker's descriptor budget. Removing
                    // them also invalidates any remaining listener events in this batch.
                    foreach (int fdToPause in listeners) poller.Remove(fdToPause);
                }
                if (errno is not (11 or 103) && now >= nextAcceptErrorLogAt)
                {
                    nextAcceptErrorLogAt = now + 5000;
                    acceptErrorLogs++;
                    Console.Error.WriteLine($"accept worker={index} errno={errno}");
                }
                break;
            }
            try
            {
                Linux.NoDelay(fd);
                var client = new Client(this, fd);
                clients.Add(fd, client);
                acceptedClients++;
                Touch(client);
                Watch(client, Linux.Read);
            }
            catch { Linux.close(fd); throw; }
        }
    }
    private void Schedule(Client client)
    {
        if (client.Closed || client.Queued) return;
        client.Queued = true;
        buffered.Enqueue(client);
        maxContinuations = Math.Max(maxContinuations, buffered.Count);
    }
    // FIN is reported as readable EOF. Do not watch RDHUP while database work
    // is pending: level-triggered half-close notifications would spin the loop.
    private void Watch(Client client, uint events) => poller.Watch(client.Fd, events);
    private void DetachTimer(Client client)
    {
        if (client.Bucket < 0) return;
        if (client.Previous is not null) client.Previous.Next = client.Next;
        else expiry[client.Bucket] = client.Next;
        if (client.Next is not null) client.Next.Previous = client.Previous;
        client.Next = client.Previous = null;
        client.Bucket = -1;
    }
    private void Touch(Client client)
    {
        DetachTimer(client);
        client.Expires = Environment.TickCount64 / 1000 + 130;
        int bucket = client.Bucket = (int)(client.Expires & 255);
        client.Next = expiry[bucket];
        if (client.Next is not null) client.Next.Previous = client;
        expiry[bucket] = client;
    }
    private void Handle(Client client, HttpRequest request)
    {
        string path = request.Path;
        int query = path.IndexOf('?');
        if (query >= 0) path = path[..query];
        DbOperation operation;
        long id = 0, userId = 0;
        string? body = null, username = null;
        if (request.Method == "GET")
        {
            if (path == "/feed") operation = DbOperation.Feed;
            else if (path == "/health") operation = DbOperation.Health;
            else if (path.StartsWith("/posts/", StringComparison.Ordinal) && !path.AsSpan(7).Contains('/'))
            {
                if (!TryId(path.AsSpan(7), out id)) { client.Error(400, "invalid post id"); return; }
                operation = DbOperation.Post;
            }
            else { client.Error(404, "not found"); return; }
        }
        else if (request.Method == "POST" && (path == "/posts" || IsLike(path)))
        {
            string? error = Auth.Validate(request.Authorization, key, out userId, out username);
            if (error is not null) { client.Error(401, error); return; }
            if (path == "/posts")
            {
                try
                {
                    using var json = JsonDocument.Parse(request.Body);
                    body = json.RootElement.ValueKind == JsonValueKind.Object && json.RootElement.TryGetProperty("body", out var value)
                        && value.ValueKind == JsonValueKind.String ? value.GetString()!.Trim() : "";
                }
                catch (JsonException) { client.Error(400, "malformed JSON body"); return; }
                if (body.Length == 0) { client.Error(400, "body is required"); return; }
                if (body.EnumerateRunes().Count() > 500) { client.Error(400, "body must be at most 500 characters"); return; }
                operation = DbOperation.Create;
            }
            else
            {
                if (!TryId(path.AsSpan(7, path.Length - 12), out id)) { client.Error(400, "invalid post id"); return; }
                operation = DbOperation.Like;
            }
        }
        else { client.Error(404, "not found"); return; }
        if (!database.Submit(new(operation, id, userId, body, username, client.Respond)))
            client.Respond(AdmissionFailure(operation));
    }
    internal static DbResponse AdmissionFailure(DbOperation operation) => operation == DbOperation.Health ? HealthBusy : BusyResponse;
    private static bool IsLike(string path) => path.StartsWith("/posts/", StringComparison.Ordinal)
        && path.EndsWith("/like", StringComparison.Ordinal) && path.Length > 12 && !path.AsSpan(7, path.Length - 12).Contains('/');
    private static bool TryId(ReadOnlySpan<char> text, out long id) => long.TryParse(text, NumberStyles.None,
        CultureInfo.InvariantCulture, out id) && id > 0;
    public void Dispose()
    {
        foreach (var client in clients.Values.ToArray()) client.Close();
        foreach (var fd in listeners) { poller.Remove(fd); Linux.close(fd); }
        database.Dispose();
        poller.Dispose();
        Console.WriteLine($"worker={index} responses={completed} accepted_clients={acceptedClients} max_continuations={maxContinuations} listener_backoffs={listenerBackoffs} accept_error_logs={acceptErrorLogs}");
    }

    private sealed unsafe class Client(Worker worker, int fd)
    {
        public int Fd { get; } = fd;
        public bool Closed, Busy, PeerEnded, Queued;
        public long Expires;
        public int Bucket = -1;
        public Client? Next, Previous;
        private byte[]? input, output;
        public int InputCount;
        private int sent;
        private bool gzip, closeAfter;
        public void Read()
        {
            input ??= ArrayPool<byte>.Shared.Rent(4096);
            // A partial request is bounded independently from the idle socket count.
            for (int reads = 0; reads < 16; reads++)
            {
                if (InputCount == input.Length)
                {
                    if (input.Length >= 65536) { Error(413, "request too large", close: true); return; }
                    var larger = ArrayPool<byte>.Shared.Rent(input.Length * 2);
                    input.AsSpan(0, InputCount).CopyTo(larger);
                    ArrayPool<byte>.Shared.Return(input, true);
                    input = larger;
                }
                nint read;
                fixed (byte* p = input) read = Linux.recv(Fd, p + InputCount, (nuint)(input.Length - InputCount), 0);
                if (read > 0)
                {
                    InputCount += (int)read;
                    worker.Touch(this);
                    Dispatch();
                    if (Closed || Busy) return;
                    input ??= ArrayPool<byte>.Shared.Rent(4096);
                    continue;
                }
                if (read == 0)
                {
                    PeerEnded = true;
                    worker.Watch(this, 0);
                    if (InputCount == 0) Close();
                    else worker.Schedule(this);
                    return;
                }
                int errno = Marshal.GetLastPInvokeError();
                if (errno == 4) continue;
                if (errno != 11) Close();
                else if (InputCount == 0) ReleaseInput();
                return;
            }
            // Bound work even when every request completes synchronously.
            if (InputCount == 0) ReleaseInput();
            else worker.Schedule(this);
        }
        public void Dispatch()
        {
            if (Closed || Busy || input is null || InputCount == 0) return;
            int parsed = HttpParser.Parse(input.AsSpan(0, InputCount), out var request);
            if (parsed == 0)
            {
                if (PeerEnded) Error(400, "incomplete request", close: true);
                return;
            }
            if (parsed < 0) { Error(400, "bad request", close: true); return; }
            gzip = request!.Gzip;
            closeAfter = request.Close;
            int remaining = InputCount - parsed;
            input.AsSpan(parsed, remaining).CopyTo(input);
            InputCount = remaining;
            if (remaining == 0) ReleaseInput();
            Busy = true;
            worker.Watch(this, 0);
            try { worker.Handle(this, request); }
            catch (Exception e)
            {
                Console.Error.WriteLine($"request failure: {e.GetType().Name}");
                Error(500, "internal server error");
            }
        }
        public void Error(int status, string message, bool close = false)
        {
            closeAfter |= close;
            Respond(new(status, Encoding.UTF8.GetBytes($"{{\"error\":\"{message}\"}}")));
        }
        public void Respond(DbResponse response)
        {
            if (Closed) return;
            Busy = true;
            byte[] body = response.Body;
            bool compressed = gzip && body.Length >= 1024;
            if (compressed)
            {
                using var memory = new MemoryStream();
                using (var zip = new GZipStream(memory, CompressionLevel.Fastest, leaveOpen: true)) zip.Write(body);
                body = memory.ToArray();
            }
            string reason = response.Status switch { 200 => "OK", 201 => "Created", 400 => "Bad Request", 401 => "Unauthorized",
                404 => "Not Found", 413 => "Content Too Large", 503 => "Service Unavailable", _ => "Internal Server Error" };
            string header = $"HTTP/1.1 {response.Status} {reason}\r\nContent-Type: application/json\r\nContent-Length: {body.Length}\r\nVary: Accept-Encoding\r\n"
                + (compressed ? "Content-Encoding: gzip\r\n" : "")
                + (closeAfter || (PeerEnded && InputCount == 0) ? "Connection: close\r\n" : "") + "\r\n";
            int headerLength = Encoding.ASCII.GetByteCount(header);
            output = new byte[headerLength + body.Length];
            Encoding.ASCII.GetBytes(header, output);
            body.CopyTo(output, headerLength);
            sent = 0;
            Flush();
        }
        public void Flush()
        {
            if (Closed || output is null) return;
            while (sent < output.Length)
            {
                nint result;
                fixed (byte* p = output) result = Linux.send(Fd, p + sent, (nuint)(output.Length - sent), 0x4000);
                if (result > 0) { sent += (int)result; continue; }
                int errno = Marshal.GetLastPInvokeError();
                if (result < 0 && errno == 4) continue;
                if (result < 0 && errno == 11) { worker.Watch(this, Linux.Write); return; }
                Close(); return;
            }
            output = null;
            Busy = false;
            worker.completed++;
            if (closeAfter || (PeerEnded && InputCount == 0)) { Close(); return; }
            worker.Touch(this);
            worker.Watch(this, PeerEnded ? 0 : Linux.Read);
            if (InputCount > 0) worker.Schedule(this);
        }
        private void ReleaseInput()
        {
            if (input is not null) ArrayPool<byte>.Shared.Return(input, clearArray: true);
            input = null;
        }
        public void Close()
        {
            if (Closed) return;
            Closed = true;
            worker.DetachTimer(this);
            worker.poller.Remove(Fd);
            Linux.close(Fd);
            worker.clients.Remove(Fd);
            ReleaseInput();
            output = null;
        }
    }
}
