// HS256 bearer tokens, checked on every request: shape, alg, signature (constant-time), exp (and nbf when present),
// then the payload claims. Nothing about a token is remembered; each loop thread keeps only its HMAC instance.
using System.Buffers;
using System.Buffers.Text;
using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace FeedServer;

enum AuthResult { Ok, Missing, Invalid, BadPayload }

readonly struct AuthUser(long id, byte[] username)
{
    public readonly long Id = id;
    public readonly byte[] Username = username; // UTF-8, unescaped
}

static class Jwt
{
    public static byte[] Key = [];

    public static ReadOnlySpan<byte> Message(AuthResult r) => r switch
    {
        AuthResult.Missing => "missing bearer token"u8,
        AuthResult.BadPayload => "invalid token payload"u8,
        _ => "invalid or expired token"u8,
    };

    public static AuthResult Verify(IncrementalHash hmac, in Request req, out AuthUser user)
    {
        user = default;
        if (!req.HasAuthorization || !req.Authorization.StartsWith("Bearer "u8)) return AuthResult.Missing;
        var token = req.Authorization[7..].Trim((byte)' ');
        if (token.Length > 8192) return AuthResult.Invalid;

        // exactly three non-empty segments: header.payload.signature
        int first = token.IndexOf((byte)'.'), last = token.LastIndexOf((byte)'.');
        if (first <= 0 || last <= first + 1 || last == token.Length - 1) return AuthResult.Invalid;
        var signed = token[..last];
        if (signed[(first + 1)..].Contains((byte)'.')) return AuthResult.Invalid;

        if (!AlgIsHs256(token[..first])) return AuthResult.Invalid;

        Span<byte> mac = stackalloc byte[32], sent = stackalloc byte[48];
        if (!Base64Url.TryDecodeFromUtf8(token[(last + 1)..], sent, out int sentLen) || sentLen != mac.Length)
            return AuthResult.Invalid;
        hmac.AppendData(signed);
        hmac.TryGetHashAndReset(mac, out _);
        if (!CryptographicOperations.FixedTimeEquals(mac, sent[..sentLen])) return AuthResult.Invalid;

        Span<byte> payload = stackalloc byte[6144];
        if (!Base64Url.TryDecodeFromUtf8(signed[(first + 1)..], payload, out int n)) return AuthResult.Invalid;
        return ReadPayload(payload[..n], out user);
    }

    // The header is a JSON object whose "alg" is "HS256".
    static bool AlgIsHs256(ReadOnlySpan<byte> headerB64)
    {
        byte[] json = ArrayPool<byte>.Shared.Rent(Base64Url.GetMaxDecodedLength(headerB64.Length));
        try
        {
            if (!Base64Url.TryDecodeFromUtf8(headerB64, json, out int n)) return false;
            using var doc = JsonDocument.Parse(json.AsMemory(0, n));
            return doc.RootElement.ValueKind == JsonValueKind.Object
                && doc.RootElement.TryGetProperty("alg"u8, out var alg)
                && alg.ValueKind == JsonValueKind.String
                && alg.ValueEquals("HS256"u8);
        }
        catch (JsonException) { return false; }
        finally { ArrayPool<byte>.Shared.Return(json); }
    }

    // Claims: exp (required) and nbf (optional) are NumericDates; sub must be a positive integer in a string and
    // username a string, otherwise the token is valid but its payload isn't.
    static AuthResult ReadPayload(ReadOnlySpan<byte> payload, out AuthUser user)
    {
        user = default;
        byte[] copy = ArrayPool<byte>.Shared.Rent(payload.Length);
        payload.CopyTo(copy);
        try
        {
            using var doc = JsonDocument.Parse(copy.AsMemory(0, payload.Length));
            var claims = doc.RootElement;
            if (claims.ValueKind != JsonValueKind.Object) return AuthResult.Invalid;
            double now = (DateTime.UtcNow - DateTime.UnixEpoch).TotalSeconds;
            if (!claims.TryGetProperty("exp"u8, out var exp) || exp.ValueKind != JsonValueKind.Number || exp.GetDouble() <= now)
                return AuthResult.Invalid;
            if (claims.TryGetProperty("nbf"u8, out var nbf) && (nbf.ValueKind != JsonValueKind.Number || nbf.GetDouble() > now))
                return AuthResult.Invalid;
            if (!claims.TryGetProperty("sub"u8, out var sub) || sub.ValueKind != JsonValueKind.String
                || !IsUserId(sub.GetString()!, out long id)) return AuthResult.BadPayload;
            if (!claims.TryGetProperty("username"u8, out var name) || name.ValueKind != JsonValueKind.String)
                return AuthResult.BadPayload;
            user = new AuthUser(id, Encoding.UTF8.GetBytes(name.GetString()!));
            return AuthResult.Ok;
        }
        catch (Exception ex) when (ex is JsonException or InvalidOperationException or FormatException)
        {
            return AuthResult.Invalid;
        }
        finally { ArrayPool<byte>.Shared.Return(copy); }
    }

    // 1 to 19 ASCII digits, positive, fits a long
    static bool IsUserId(string s, out long id)
    {
        id = 0;
        return s.Length is > 0 and <= 19 && s.All(char.IsAsciiDigit)
            && long.TryParse(s, NumberStyles.None, CultureInfo.InvariantCulture, out id) && id > 0;
    }
}
