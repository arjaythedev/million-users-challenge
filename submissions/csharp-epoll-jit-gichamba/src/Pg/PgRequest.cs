namespace Feed.Pg;

/// <summary>
/// One prepared statement to run on a pipelined connection; its Sync may be shared with other requests. Callbacks run
/// on the loop thread that owns the connection and must not block.
/// </summary>
public abstract class PgRequest {
    /// <summary>Creates a request for the statement prepared as <paramref name="preparedName"/>.</summary>
    protected PgRequest(string preparedName) => StatementName = preparedName;

    /// <summary>The prepared name of the statement.</summary>
    internal string StatementName { get; }

    /// <summary>The statement's parameters, to fill before sending.</summary>
    public PgParams Params { get; } = new();

    /// <summary>A result row.</summary>
    public abstract void OnRow(in PgRow row);

    /// <summary>The statement finished; <paramref name="tag"/> is the command tag such as "INSERT 0 1".</summary>
    public abstract void OnCommandComplete(ReadOnlySpan<byte> tag);

    /// <summary>The server reported an error. <see cref="OnDone"/> follows.</summary>
    public abstract void OnError(PgError error);

    /// <summary>
    /// The result is final, with or without an error: a read's at its own result, a write's once its group committed.
    /// </summary>
    public abstract void OnDone();
}
