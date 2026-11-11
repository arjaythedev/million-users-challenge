using System.Buffers;
using System.Buffers.Text;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Unicode;

namespace MillionReactor;

internal static class Auth
{
    public const string InvalidPayload = "invalid token payload";
    private const string InvalidToken = "invalid or expired token";
    private const int StackBufferSize = 1024;
    private const int MaxTokenLength = 32768;

    // Every call verifies the original signing input. No token or claims cache.
    public static string? Validate(string? authorization, ReadOnlySpan<byte> key,
        out long userId, out string? username)
    {
        userId = 0;
        username = null;
        if (authorization is null || !authorization.StartsWith("Bearer ", StringComparison.Ordinal))
            return "missing bearer token";
        ReadOnlySpan<char> token = authorization.AsSpan(7);
        if (token.Length is 0 or > MaxTokenLength)
            return InvalidToken;
        int firstDot = token.IndexOf('.');
        if (firstDot <= 0)
            return InvalidToken;
        int secondDot = token[(firstDot + 1)..].IndexOf('.');
        if (secondDot <= 0)
            return InvalidToken;
        secondDot += firstDot + 1;
        if (token.Length - secondDot - 1 != 43)
            return InvalidToken;

        byte[]? rentedInput = null;
        byte[]? rentedDecoded = null;
        Span<byte> input = token.Length <= StackBufferSize
            ? stackalloc byte[StackBufferSize]
            : (rentedInput = ArrayPool<byte>.Shared.Rent(token.Length));
        Span<byte> decoded = token.Length <= StackBufferSize
            ? stackalloc byte[StackBufferSize]
            : (rentedDecoded = ArrayPool<byte>.Shared.Rent(token.Length));
        try
        {
            // JWT uses unpadded base64url. Reject whitespace, extra dots, and
            // non-ASCII before the decoder (which otherwise accepts padding).
            for (int i = 0; i < token.Length; i++)
            {
                char c = token[i];
                if (i != firstDot && i != secondDot &&
                    !(c is >= 'A' and <= 'Z' or >= 'a' and <= 'z' or >= '0' and <= '9' or '-' or '_'))
                    return InvalidToken;
                input[i] = (byte)c;
            }
            Span<byte> signature = stackalloc byte[HMACSHA256.HashSizeInBytes];
            Span<byte> expected = stackalloc byte[HMACSHA256.HashSizeInBytes];
            if (!Base64Url.TryDecodeFromUtf8(input.Slice(secondDot + 1, 43), signature, out int size) ||
                size != HMACSHA256.HashSizeInBytes)
                return InvalidToken;
            HMACSHA256.HashData(key, input[..secondDot], expected);
            if (!CryptographicOperations.FixedTimeEquals(expected, signature))
                return InvalidToken;
            if (!Base64Url.TryDecodeFromUtf8(input[..firstDot], decoded, out size) ||
                !Utf8.IsValid(decoded[..size]) || !ValidHeader(decoded[..size]))
                return InvalidToken;
            if (!Base64Url.TryDecodeFromUtf8(input.Slice(firstDot + 1, secondDot - firstDot - 1), decoded, out size) ||
                !Utf8.IsValid(decoded[..size]))
                return InvalidToken;
            string? error = ReadClaims(decoded[..size], out long id, out string? name);
            if (error is not null)
                return error;
            userId = id;
            username = name;
            return null;
        }
        catch (JsonException) { return InvalidToken; }
        catch (FormatException) { return InvalidToken; }
        catch (InvalidOperationException) { return InvalidToken; }
        finally
        {
            if (rentedInput is not null)
                ArrayPool<byte>.Shared.Return(rentedInput, clearArray: true);
            if (rentedDecoded is not null)
                ArrayPool<byte>.Shared.Return(rentedDecoded, clearArray: true);
        }
    }

    private static bool ValidHeader(ReadOnlySpan<byte> json)
    {
        var reader = new Utf8JsonReader(json);
        if (!reader.Read() || reader.TokenType != JsonTokenType.StartObject)
            return false;
        bool foundAlgorithm = false, foundCritical = false, foundB64 = false;
        while (reader.Read() && reader.TokenType != JsonTokenType.EndObject)
        {
            if (reader.TokenType != JsonTokenType.PropertyName)
                return false;
            bool algorithm = reader.ValueTextEquals("alg"u8);
            bool critical = reader.ValueTextEquals("crit"u8);
            bool b64 = reader.ValueTextEquals("b64"u8);
            if (!reader.Read())
                return false;
            if (algorithm)
            {
                if (foundAlgorithm || reader.TokenType != JsonTokenType.String || !reader.ValueTextEquals("HS256"u8))
                    return false;
                foundAlgorithm = true;
            }
            else if (critical)
            {
                // No JWS extensions are implemented.
                if (foundCritical || reader.TokenType != JsonTokenType.StartArray || !reader.Read() ||
                    reader.TokenType != JsonTokenType.EndArray)
                    return false;
                foundCritical = true;
            }
            else if (b64)
            {
                if (foundB64 || reader.TokenType != JsonTokenType.True)
                    return false;
                foundB64 = true;
            }
            reader.Skip();
        }
        return foundAlgorithm && reader.TokenType == JsonTokenType.EndObject && !reader.Read();
    }

    private static string? ReadClaims(ReadOnlySpan<byte> json, out long userId, out string? username)
    {
        userId = 0;
        username = null;
        var reader = new Utf8JsonReader(json);
        if (!reader.Read() || reader.TokenType != JsonTokenType.StartObject)
            return InvalidToken;
        bool foundSub = false, foundName = false, foundExpiry = false, foundNotBefore = false;
        bool validIdentity = true;
        double expiry = 0, notBefore = 0;
        while (reader.Read() && reader.TokenType != JsonTokenType.EndObject)
        {
            if (reader.TokenType != JsonTokenType.PropertyName)
                return InvalidToken;
            bool sub = reader.ValueTextEquals("sub"u8);
            bool name = reader.ValueTextEquals("username"u8);
            bool exp = reader.ValueTextEquals("exp"u8);
            bool nbf = reader.ValueTextEquals("nbf"u8);
            if (!reader.Read())
                return InvalidToken;
            if (exp)
            {
                if (foundExpiry || reader.TokenType != JsonTokenType.Number ||
                    !reader.TryGetDouble(out expiry) || !double.IsFinite(expiry))
                    return InvalidToken;
                foundExpiry = true;
            }
            else if (nbf)
            {
                if (foundNotBefore || reader.TokenType != JsonTokenType.Number ||
                    !reader.TryGetDouble(out notBefore) || !double.IsFinite(notBefore))
                    return InvalidToken;
                foundNotBefore = true;
            }
            else if (sub)
            {
                if (foundSub)
                    validIdentity = false;
                foundSub = true;
                validIdentity &= reader.TokenType == JsonTokenType.String &&
                    (reader.ValueIsEscaped
                        ? TryParseId(reader.GetString().AsSpan(), out userId)
                        : TryParseId(reader.ValueSpan, out userId));
            }
            else if (name)
            {
                if (foundName)
                    validIdentity = false;
                foundName = true;
                if (reader.TokenType == JsonTokenType.String)
                    username = reader.GetString();
                else
                    validIdentity = false;
            }
            reader.Skip();
        }
        if (reader.TokenType != JsonTokenType.EndObject || reader.Read())
            return InvalidToken;
        double now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() / 1000d;
        if (!foundExpiry || expiry <= now || (foundNotBefore && notBefore > now))
            return InvalidToken;
        return validIdentity && foundSub && foundName && userId > 0 ? null : InvalidPayload;
    }

    private static bool TryParseId(ReadOnlySpan<byte> raw, out long id)
    {
        id = 0;
        foreach (byte c in raw)
        {
            int digit = c - (byte)'0';
            if ((uint)digit > 9 || id > (long.MaxValue - digit) / 10)
                return false;
            id = id * 10 + digit;
        }
        return id > 0;
    }

    private static bool TryParseId(ReadOnlySpan<char> raw, out long id)
    {
        id = 0;
        foreach (char c in raw)
        {
            int digit = c - '0';
            if ((uint)digit > 9 || id > (long.MaxValue - digit) / 10)
                return false;
            id = id * 10 + digit;
        }
        return id > 0;
    }
}
