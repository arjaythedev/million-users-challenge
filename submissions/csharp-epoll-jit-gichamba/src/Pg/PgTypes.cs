namespace Feed.Pg;

/// <summary>A Postgres error: severity, SQLSTATE and message from an ErrorResponse.</summary>
public readonly record struct PgError(string Severity, string SqlState, string Message);

/// <summary>Thrown for a server error during login or prepare, or for a protocol violation.</summary>
public sealed class PgException : Exception {
    /// <summary>The server error, or an empty-state error for local protocol failures.</summary>
    public PgError Error { get; }

    /// <summary>Wraps a server error.</summary>
    public PgException(PgError error) : base($"{error.SqlState}: {error.Message}") => Error = error;

    /// <summary>A local protocol failure with no server error.</summary>
    public PgException(string message) : base(message) => Error = new PgError("", "", message);
}

/// <summary>Type OIDs used for Parse parameter types.</summary>
public static class PgOid {
    /// <summary>int8</summary>
    public const uint Int8 = 20;
    /// <summary>text</summary>
    public const uint Text = 25;
}

/// <summary>Byte layouts and tags of backend messages used by the reader loop.</summary>
internal static class PgMessages {
    /// <summary>Decodes the field list of an ErrorResponse or NoticeResponse body.</summary>
    public static PgError ParseError(ReadOnlySpan<byte> body) {
        string severity = "", state = "", message = "";
        int i = 0;
        while (i < body.Length && body[i] != 0) {
            byte code = body[i++];
            int end = body[i..].IndexOf((byte)0);
            if (end < 0) {
                throw new PgException("unterminated error field");
            }

            string value = System.Text.Encoding.UTF8.GetString(body.Slice(i, end));
            i += end + 1;
            switch ((char)code) {
                case 'S': severity = value; break;
                case 'C': state = value; break;
                case 'M': message = value; break;
            }
        }
        return new PgError(severity, state, message);
    }

    /// <summary>Returns the command tag text of a CommandComplete body, without the NUL.</summary>
    public static ReadOnlySpan<byte> ParseTag(ReadOnlySpan<byte> body) {
        int end = body.IndexOf((byte)0);
        return end < 0 ? body : body[..end];
    }
}
