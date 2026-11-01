using System.Buffers.Binary;

namespace Feed.Pg;

/// <summary>Decoders for values in binary result format. Callers check <see cref="PgRow.IsNull"/> first.</summary>
public static class PgBinary {
    /// <summary>Reads an int4.</summary>
    public static int ReadInt4(ReadOnlySpan<byte> v) => BinaryPrimitives.ReadInt32BigEndian(v);

    /// <summary>Reads an int8.</summary>
    public static long ReadInt8(ReadOnlySpan<byte> v) => BinaryPrimitives.ReadInt64BigEndian(v);

    /// <summary>Reads a timestamptz as microseconds since 2000-01-01T00:00:00Z.</summary>
    public static long ReadTimestamptz(ReadOnlySpan<byte> v) => ReadInt8(v);
}
