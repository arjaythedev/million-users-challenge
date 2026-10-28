namespace Feed.Pg;

/// <summary>Binary-encoded Bind parameters for one statement. Reusable through <see cref="Clear"/>.</summary>
public sealed class PgParams {
    /// <summary>Holds the encoded values. Most lists are one or two ids; a new post's body grows it.</summary>
    private readonly PgWriter _w = new(64);

    /// <summary>Number of parameters added.</summary>
    public int Count { get; private set; }

    /// <summary>The encoded values: for each parameter an int32 length followed by the bytes.</summary>
    internal ReadOnlySpan<byte> Values => _w.Written;

    /// <summary>Removes all parameters.</summary>
    public void Clear() {
        _w.Clear();
        Count = 0;
    }

    /// <summary>Adds an int8.</summary>
    public void AddInt8(long v) {
        _w.Int32(8);
        _w.Int64(v);
        Count++;
    }

    /// <summary>Adds text from a string, as UTF-8.</summary>
    public void AddText(string v) {
        int slot = _w.ReserveInt32();
        _w.Utf8(v);
        _w.PatchLengthSince(slot);
        Count++;
    }
}
