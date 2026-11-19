using System.Globalization;
using System.Text;

namespace Feed;

/// <summary>
/// Thread-safe counters and the periodic stats line on stderr, which the service journal keeps. Other components call
/// the Record methods; <see cref="Report"/> turns the change since the last report into one line.
/// </summary>
public sealed class Stats {
    /// <summary>Endpoint names, in report order.</summary>
    private readonly string[] _endpoints;
    /// <summary>Requests counted per endpoint since start.</summary>
    private readonly long[] _requests;
    /// <summary>Per-endpoint request counts at the last report, so a report can give the interval's rate.</summary>
    private readonly long[] _lastRequests;
    /// <summary>Totals since start: connections refused for lack of descriptors, Postgres errors and accept errors.</summary>
    private long _emfile, _pgErrors, _acceptErrors;
    /// <summary>Text of the latest Postgres error not yet reported, or null.</summary>
    private string? _lastError;

    /// <summary>Creates counters for the named endpoints, in the order they are reported.</summary>
    public Stats(string[] endpoints) {
        _endpoints = endpoints;
        _requests = new long[endpoints.Length];
        _lastRequests = new long[endpoints.Length];
    }

    /// <summary>Connections refused for lack of descriptors since start: EMFILE or ENFILE from accept, or past the reserve.</summary>
    public long Emfile => Interlocked.Read(ref _emfile);
    /// <summary>Postgres failures since start: query errors other than expected ones, lost requests and failed connects.</summary>
    public long PgErrors => Interlocked.Read(ref _pgErrors);
    /// <summary>Total accept-path failures that were survived since start, such as a failed socket option, a connection that could not be registered or an exception caught in a loop handler.</summary>
    public long AcceptErrors => Interlocked.Read(ref _acceptErrors);

    /// <summary>Counts one request to endpoint index <paramref name="endpoint"/>.</summary>
    public void RecordRequest(int endpoint) => Interlocked.Increment(ref _requests[endpoint]);

    /// <summary>Counts one connection refused for lack of descriptors.</summary>
    public void RecordEmfile() => Interlocked.Increment(ref _emfile);

    /// <summary>Counts one Postgres failure and keeps <paramref name="message"/> as the latest error.</summary>
    public void RecordPgError(string message) {
        Interlocked.Increment(ref _pgErrors);
        Volatile.Write(ref _lastError, message);
    }

    /// <summary>Counts one accept-path failure that was survived.</summary>
    public void RecordAcceptError() => Interlocked.Increment(ref _acceptErrors);

    /// <summary>Returns the latest Postgres error text since the last call, or null when there was none.</summary>
    public string? TakeLastError() => Interlocked.Exchange(ref _lastError, null);

    /// <summary>
    /// Builds the stats line for the last <paramref name="elapsedSeconds"/> seconds and starts the next interval.
    /// Rates are per second; the error figures are totals since start. Call it from one thread: the interval figures
    /// are not locked.
    /// </summary>
    public string Report(double elapsedSeconds) {
        CultureInfo inv = CultureInfo.InvariantCulture;
        StringBuilder sb = new("stats rps");
        for (int i = 0; i < _endpoints.Length; i++) {
            long now = Interlocked.Read(ref _requests[i]);
            sb.Append(' ').Append(_endpoints[i]).Append('=').Append(Rate(now - _lastRequests[i], elapsedSeconds).ToString(inv));
            _lastRequests[i] = now;
        }
        sb.Append(" emfile=").Append(Emfile.ToString(inv));
        sb.Append(" pg_err=").Append(PgErrors.ToString(inv));
        sb.Append(" accept_err=").Append(AcceptErrors.ToString(inv));
        return sb.ToString();
    }

    /// <summary>Starts a background thread that writes <see cref="Report"/> to stderr every <paramref name="seconds"/> seconds.</summary>
    public void StartReporter(int seconds) {
        Thread thread = new(() => {
            long last = Environment.TickCount64;
            while (true) {
                Thread.Sleep(seconds * 1000);
                long now = Environment.TickCount64;
                WriteReport((now - last) / 1000.0, Console.Error.WriteLine);
                last = now;
            }
        }) { IsBackground = true, Name = "stats" };
        thread.Start();
    }

    /// <summary>
    /// Writes one report line over <paramref name="seconds"/>, then the latest error if there is one. Never throws: on
    /// the reporter thread an exception would end the process. A failed write loses that error's text, not its count.
    /// </summary>
    internal void WriteReport(double seconds, Action<string> write) {
        string line = Report(seconds);
        string? lastError = TakeLastError();
        try {
            write(line);
            if (lastError is not null) {
                write("last error: " + lastError);
            }
        } catch (Exception) {
            // stderr is unusable for now; the counters keep counting and the next interval tries again.
        }
    }

    /// <summary>Turns a count over <paramref name="seconds"/> into whole units per second, rounding halves up.</summary>
    private static long Rate(long delta, double seconds) => (long)Math.Round(delta / seconds, MidpointRounding.AwayFromZero);
}
