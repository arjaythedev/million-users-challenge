using System.Runtime.InteropServices;

namespace MillionReactor;

// libpq owns its socket and PGresult memory. Callers must never close a PQsocket
// directly, and must PQclear every non-null result exactly once.
internal static unsafe partial class PgNative
{
    private const string LibPq = "libpq.so.5";

    [LibraryImport(LibPq)] internal static partial nint PQconnectStartParams(byte** keywords, byte** values, int expandDbname);
    [LibraryImport(LibPq)] internal static partial int PQconnectPoll(nint connection);
    [LibraryImport(LibPq)] internal static partial int PQstatus(nint connection);
    [LibraryImport(LibPq)] internal static partial int PQsocket(nint connection);
    [LibraryImport(LibPq)] internal static partial int PQsetnonblocking(nint connection, int nonblocking);
    [LibraryImport(LibPq)] internal static partial int PQenterPipelineMode(nint connection);
    [LibraryImport(LibPq)] internal static partial int PQpipelineSync(nint connection);
    [LibraryImport(LibPq)] internal static partial int PQflush(nint connection);
    [LibraryImport(LibPq)] internal static partial int PQconsumeInput(nint connection);
    [LibraryImport(LibPq)] internal static partial int PQisBusy(nint connection);
    [LibraryImport(LibPq)] internal static partial nint PQgetResult(nint connection);
    [LibraryImport(LibPq)] internal static partial void PQfinish(nint connection);
    [LibraryImport(LibPq)] internal static partial void PQclear(nint result);
    [LibraryImport(LibPq)] internal static partial int PQresultStatus(nint result);
    [LibraryImport(LibPq)] internal static partial int PQntuples(nint result);
    [LibraryImport(LibPq)] internal static partial int PQnfields(nint result);
    [LibraryImport(LibPq)] internal static partial int PQgetisnull(nint result, int row, int column);
    [LibraryImport(LibPq)] internal static partial int PQgetlength(nint result, int row, int column);
    [LibraryImport(LibPq)] internal static partial byte* PQgetvalue(nint result, int row, int column);
    [LibraryImport(LibPq)] internal static partial byte* PQcmdTuples(nint result);
    [LibraryImport(LibPq)] internal static partial nint PQresultErrorField(nint result, int fieldCode);
    [LibraryImport(LibPq)] internal static partial int PQsendPrepare(nint connection, byte* name, byte* sql, int parameterCount, uint* parameterTypes);
    [LibraryImport(LibPq)] internal static partial int PQsendQueryPrepared(nint connection, byte* name, int parameterCount,
        byte** values, int* lengths, int* formats, int resultFormat);

    [StructLayout(LayoutKind.Sequential)]
    internal struct PollFd
    {
        internal int Fd;
        internal short Events;
        internal short ReturnedEvents;
    }

    // Used only while the shard constructor waits for initial connections. Request
    // and reconnection I/O is driven by the caller's epoll loop, never by poll/waits.
    [LibraryImport("libc.so.6", SetLastError = true)]
    internal static partial int poll(PollFd* fds, nuint count, int timeoutMilliseconds);
}

