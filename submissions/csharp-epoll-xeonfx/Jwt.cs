using System;
using System.Buffers;
using System.Buffers.Text;
using System.Security.Cryptography;
using System.Text.Json;

enum TokenCheck : byte
{
    Valid,
    /// <summary>No header, or not "Bearer " and a token.</summary>
    Missing,
    /// <summary>Not signed with the secret, not HS256, malformed, or past its exp.</summary>
    Invalid,
    /// <summary>Genuine and in time, but sub isn't a positive integer string or username isn't a string.</summary>
    InvalidPayload,
}

/// <summary>HS256 JWTs, verified on every request: signature, then exp, then sub and username.</summary>
sealed class Jwt(byte[] secret)
{
    public const int MaxTokenBytes = 4096;
    /// <summary>The longest username a token can carry, unescaped.</summary>
    public const int MaxNameBytes = MaxTokenBytes / 4 * 3;

    /// <summary>{"alg":"HS256","typ":"JWT"}, the header nearly every token carries: no need to decode and parse it.</summary>
    static ReadOnlySpan<byte> Hs256Header => "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"u8;

    static readonly SearchValues<byte> TokenChars = SearchValues.Create("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_."u8);

    /// <summary>The Authorization header's token's worth; when valid, the user id and the username, copied into
    /// <paramref name="name"/> (<see cref="MaxNameBytes"/> long).</summary>
    public TokenCheck Check(ReadOnlySpan<byte> header, Span<byte> name, out long sub, out int nameLength)
    {
        sub = 0;
        nameLength = 0;
        header = header.Trim(" \t"u8);
        if (!header.StartsWith("Bearer "u8)) return TokenCheck.Missing;
        ReadOnlySpan<byte> token = header[7..];
        if (token.Length is 0 or > MaxTokenBytes || token.ContainsAnyExcept(TokenChars)) return TokenCheck.Invalid;
        // header.payload.signature
        int firstDot = token.IndexOf((byte)'.');
        if (firstDot <= 0) return TokenCheck.Invalid;
        int secondDot = token[(firstDot + 1)..].IndexOf((byte)'.');
        if (secondDot <= 0) return TokenCheck.Invalid;
        secondDot += firstDot + 1;
        ReadOnlySpan<byte> signature = token[(secondDot + 1)..];
        if (signature.IndexOf((byte)'.') >= 0) return TokenCheck.Invalid;

        // signature first: nothing in an unauthenticated token is worth parsing
        Span<byte> mac = stackalloc byte[32];
        HMACSHA256.HashData(secret, token[..secondDot], mac);
        Span<byte> given = stackalloc byte[48];
        if (!Decode(signature, given, out int signatureLength) || signatureLength != 32 || !CryptographicOperations.FixedTimeEquals(mac, given[..32]))
            return TokenCheck.Invalid;

        Span<byte> buffer = stackalloc byte[MaxTokenBytes / 4 * 3];
        if (!token[..firstDot].SequenceEqual(Hs256Header)
            && (!Decode(token[..firstDot], buffer, out int headerLength) || !HeaderIsHs256(buffer[..headerLength]))) return TokenCheck.Invalid;
        if (!Decode(token[(firstDot + 1)..secondDot], buffer, out int payloadLength)) return TokenCheck.Invalid;
        var check = Payload(buffer[..payloadLength], DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() / 1000.0, name, out sub, out nameLength);
        if (check != TokenCheck.Valid) sub = 0;
        return check;
    }

    /// <summary>Base64url without throwing.</summary>
    static bool Decode(ReadOnlySpan<byte> s, Span<byte> dest, out int written) =>
        Base64Url.DecodeFromUtf8(s, dest, out _, out written) == OperationStatus.Done;

    static bool HeaderIsHs256(ReadOnlySpan<byte> json)
    {
        try
        {
            var r = new Utf8JsonReader(json);
            if (!r.Read() || r.TokenType != JsonTokenType.StartObject) return false;
            bool hs256 = false;
            while (r.Read() && r.TokenType == JsonTokenType.PropertyName)
            {
                bool alg = r.ValueTextEquals("alg"u8);
                if (!r.Read()) return false;
                if (alg) hs256 = r.TokenType == JsonTokenType.String && r.ValueTextEquals("HS256"u8);
                else r.Skip();
            }
            return hs256 && r.TokenType == JsonTokenType.EndObject && !r.Read();
        }
        catch (Exception e) when (e is JsonException or InvalidOperationException)
        {
            return false;
        }
    }

    /// <summary>A payload that isn't a JSON object, or whose exp isn't a number in the future, is Invalid; then a sub
    /// that isn't a positive integer string, or a username that isn't a string, is InvalidPayload.</summary>
    static TokenCheck Payload(ReadOnlySpan<byte> json, double now, Span<byte> name, out long sub, out int nameLength)
    {
        sub = 0;
        nameLength = -1;
        try
        {
            var r = new Utf8JsonReader(json);
            if (!r.Read() || r.TokenType != JsonTokenType.StartObject) return TokenCheck.Invalid;
            double exp = double.NaN;
            bool bad = false;
            while (r.Read() && r.TokenType == JsonTokenType.PropertyName)
            {
                if (r.ValueTextEquals("sub"u8))
                {
                    r.Read();
                    if (r.TokenType == JsonTokenType.String && !r.ValueIsEscaped && PositiveInteger(r.ValueSpan, out long s)) sub = s;
                    else
                    {
                        bad = true;
                        r.Skip();
                    }
                }
                else if (r.ValueTextEquals("exp"u8))
                {
                    r.Read();
                    if (r.TokenType != JsonTokenType.Number || !r.TryGetDouble(out exp)) return TokenCheck.Invalid;
                }
                else if (r.ValueTextEquals("username"u8))
                {
                    r.Read();
                    if (r.TokenType == JsonTokenType.Null) nameLength = -1;
                    else if (r.TokenType == JsonTokenType.String) nameLength = r.CopyString(name);
                    else
                    {
                        bad = true;
                        r.Skip();
                    }
                }
                else
                {
                    r.Read();
                    r.Skip();
                }
            }
            if (r.TokenType != JsonTokenType.EndObject || r.Read()) return TokenCheck.Invalid;
            if (double.IsNaN(exp) || !(now < exp)) return TokenCheck.Invalid; // exp is required
            if (bad || sub <= 0 || nameLength < 0 || !double.IsFinite(exp)) return TokenCheck.InvalidPayload;
            return TokenCheck.Valid;
        }
        catch (Exception e) when (e is JsonException or InvalidOperationException)
        {
            return TokenCheck.Invalid;
        }
    }

    /// <summary>Digits only, at least 1, within a long.</summary>
    public static bool PositiveInteger(ReadOnlySpan<byte> s, out long value)
    {
        value = 0;
        if (s.IsEmpty || s.Length > 19) return false;
        foreach (byte b in s)
        {
            if (b is < (byte)'0' or > (byte)'9') return false;
            value = value * 10 + (b - '0');
        }
        return value > 0; // 19 digits can pass long.MaxValue and wrap negative
    }
}
