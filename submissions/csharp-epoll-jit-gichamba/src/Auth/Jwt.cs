using System.Buffers;
using System.Buffers.Text;
using System.Security.Cryptography;
using System.Text.Json;

namespace Feed.Auth;

/// <summary>Result of checking a JWT.</summary>
public enum JwtStatus {
    /// <summary>Signature, algorithm, expiry and claims are good.</summary>
    Ok,
    /// <summary>The token is malformed, signed wrongly, uses another algorithm, lacks <c>exp</c> or has expired.</summary>
    Invalid,
    /// <summary>The token is valid, but <c>sub</c> is not a positive integer string or <c>username</c> is not a string.</summary>
    BadPayload,
}

/// <summary>
/// HS256 JSON Web Tokens as SPEC.md describes them: claims <c>sub</c> (decimal user id as a string), <c>username</c>,
/// <c>iat</c> and <c>exp</c>. Nothing about a token is cached between calls; each thread keeps only the HMAC state
/// keyed with the secret, so a check does not hash the key again.
/// </summary>
public static class Jwt {
    /// <summary>Longest token accepted. It also sizes the decode buffer.</summary>
    private const int MaxTokenLength = 1024;
    /// <summary>Length of an HMAC-SHA256 signature in bytes.</summary>
    private const int MacLength = 32;

    /// <summary>
    /// Checks <paramref name="token"/> (the JWT text as ASCII bytes): the HMAC-SHA256 signature in constant time, an
    /// <c>alg</c> of exactly HS256 and a numeric <c>exp</c> after <paramref name="nowUnix"/>, then a positive decimal
    /// <c>sub</c> and a string <c>username</c>. On <see cref="JwtStatus.Ok"/>, <paramref name="userId"/> and
    /// <paramref name="username"/> hold those claims.
    /// </summary>
    public static JwtStatus Verify(ReadOnlySpan<byte> token, ReadOnlySpan<byte> key, long nowUnix, out long userId, out string? username) {
        userId = 0;
        username = null;
        // Base64Url decoding tolerates '=' padding; JWTs do not carry any.
        if (token.Length == 0 || token.Length > MaxTokenLength || token.IndexOf((byte)'=') >= 0) {
            return JwtStatus.Invalid;
        }

        int dot1 = token.IndexOf((byte)'.');
        if (dot1 <= 0) {
            return JwtStatus.Invalid;
        }

        int dot2 = dot1 + 1 + token[(dot1 + 1)..].IndexOf((byte)'.');
        if (dot2 <= dot1 + 1 || dot2 == token.Length - 1 || token[(dot2 + 1)..].IndexOf((byte)'.') >= 0) {
            return JwtStatus.Invalid;
        }

        Span<byte> mac = stackalloc byte[MacLength];
        if (Base64Url.DecodeFromUtf8(token[(dot2 + 1)..], mac, out _, out int macLen) != OperationStatus.Done
            || macLen != MacLength) {
            return JwtStatus.Invalid;
        }

        Span<byte> expected = stackalloc byte[MacLength];
        Mac(key, token[..dot2], expected);
        if (!CryptographicOperations.FixedTimeEquals(mac, expected)) {
            return JwtStatus.Invalid;
        }

        Span<byte> json = stackalloc byte[MaxTokenLength];
        if (Base64Url.DecodeFromUtf8(token[..dot1], json, out _, out int headerLen) != OperationStatus.Done
            || !HeaderIsHs256(json[..headerLen])) {
            return JwtStatus.Invalid;
        }

        if (Base64Url.DecodeFromUtf8(token[(dot1 + 1)..dot2], json, out _, out int payloadLen) != OperationStatus.Done) {
            return JwtStatus.Invalid;
        }

        return ReadClaims(json[..payloadLen], nowUnix, out userId, out username);
    }

    /// <summary>This thread's keyed HMAC state, rebuilt when the key changes.</summary>
    [ThreadStatic]
    private static KeyedMac? t_mac;

    /// <summary>Writes the HMAC-SHA256 of <paramref name="data"/> under <paramref name="key"/> into <paramref name="into"/>. Each thread keeps one keyed state and resets it after each use.</summary>
    private static void Mac(ReadOnlySpan<byte> key, ReadOnlySpan<byte> data, Span<byte> into) {
        KeyedMac? m = t_mac;
        if (m is null || !key.SequenceEqual(m.Key)) {
            m?.Hmac.Dispose();
            t_mac = m = new KeyedMac(key.ToArray());
        }

        m.Hmac.AppendData(data);
        m.Hmac.GetHashAndReset(into);
    }

    /// <summary>An HMAC-SHA256 state bound to one key.</summary>
    private sealed class KeyedMac {
        /// <summary>The key this state was made for.</summary>
        public readonly byte[] Key;
        /// <summary>The reusable HMAC state.</summary>
        public readonly IncrementalHash Hmac;

        /// <summary>Keeps <paramref name="key"/> and creates an HMAC keyed with it.</summary>
        public KeyedMac(byte[] key) {
            Key = key;
            Hmac = IncrementalHash.CreateHMAC(HashAlgorithmName.SHA256, key);
        }
    }

    /// <summary>True when the header is a JSON object with an <c>alg</c> of the string HS256. A repeated <c>alg</c> must also be HS256.</summary>
    private static bool HeaderIsHs256(ReadOnlySpan<byte> json) {
        try {
            Utf8JsonReader r = new(json);
            if (!r.Read() || r.TokenType != JsonTokenType.StartObject) {
                return false;
            }

            bool seen = false;
            while (r.Read() && r.TokenType == JsonTokenType.PropertyName) {
                bool isAlg = r.ValueTextEquals("alg"u8);
                r.Read();
                if (isAlg) {
                    if (r.TokenType != JsonTokenType.String || !r.ValueTextEquals("HS256"u8)) {
                        return false;
                    }

                    seen = true;
                } else {
                    r.Skip();
                }
            }
            return seen && r.TokenType == JsonTokenType.EndObject && !r.Read();
        } catch (JsonException) {
            return false;
        }
    }

    /// <summary>
    /// Reads the top-level claims. A payload that is not a JSON object, or has no numeric <c>exp</c> after
    /// <paramref name="nowUnix"/>, is <see cref="JwtStatus.Invalid"/>; then a missing or bad <c>sub</c> or
    /// <c>username</c> is <see cref="JwtStatus.BadPayload"/>. A repeated claim keeps its last value.
    /// </summary>
    private static JwtStatus ReadClaims(ReadOnlySpan<byte> json, long nowUnix, out long sub, out string? username) {
        sub = 0;
        username = null;
        bool subOk = false;
        double exp = double.NaN;
        try {
            Utf8JsonReader r = new(json);
            if (!r.Read() || r.TokenType != JsonTokenType.StartObject) {
                return JwtStatus.Invalid;
            }

            while (r.Read() && r.TokenType == JsonTokenType.PropertyName) {
                bool isSub = r.ValueTextEquals("sub"u8);
                bool isExp = !isSub && r.ValueTextEquals("exp"u8);
                bool isName = !isSub && !isExp && r.ValueTextEquals("username"u8);
                r.Read();
                if (isSub) {
                    subOk = r.TokenType == JsonTokenType.String && ParseId(r.ValueSpan, r.ValueIsEscaped, out sub);
                } else if (isExp) {
                    exp = r.TokenType == JsonTokenType.Number && r.TryGetDouble(out double e) ? e : double.NaN;
                } else if (isName) {
                    username = r.TokenType == JsonTokenType.String ? r.GetString() : null;
                }

                // Moves past an object or array value; a no-op on a scalar.
                r.Skip();
            }
            if (r.TokenType != JsonTokenType.EndObject || r.Read()) {
                return JwtStatus.Invalid;
            }
        } catch (JsonException) {
            return JwtStatus.Invalid;
        } catch (InvalidOperationException) {
            // GetString on text that is not valid UTF-8.
            return JwtStatus.Invalid;
        }

        if (double.IsNaN(exp) || exp <= nowUnix) {
            return JwtStatus.Invalid;
        }

        if (!subOk || username is null) {
            sub = 0;
            username = null;
            return JwtStatus.BadPayload;
        }

        return JwtStatus.Ok;
    }

    /// <summary>Reads 1 to 18 plain digits as a positive id. Escaped text and zero are refused.</summary>
    private static bool ParseId(ReadOnlySpan<byte> digits, bool escaped, out long id) {
        id = 0;
        if (escaped || digits.Length is 0 or > 18) {
            return false;
        }

        foreach (byte b in digits) {
            if (b < '0' || b > '9') {
                return false;
            }

            id = id * 10 + (b - '0');
        }
        return id > 0;
    }
}
