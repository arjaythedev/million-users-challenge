using System.Text;
using System.Text.Json;

namespace Feed.Api;

/// <summary>Outcome of checking a POST /posts request body.</summary>
public enum PostBodyStatus {
    /// <summary>The body text is usable.</summary>
    Ok,
    /// <summary>The request body is not valid JSON.</summary>
    Malformed,
    /// <summary><c>body</c> is missing, not a string, or empty after trimming.</summary>
    Required,
    /// <summary><c>body</c> is longer than <see cref="PostBody.MaxCharacters"/> characters after trimming.</summary>
    TooLong,
}

/// <summary>Reads and checks the JSON body of POST /posts: <c>{"body":"..."}</c>.</summary>
public static class PostBody {
    /// <summary>The longest body allowed, in characters (Unicode code points, as Postgres's char_length counts them).</summary>
    public const int MaxCharacters = 500;

    /// <summary>
    /// Parses <paramref name="json"/> and returns the <c>body</c> string with surrounding whitespace trimmed in
    /// <paramref name="text"/>. The whole document must be valid JSON. A document that is not an object has no
    /// <c>body</c>. A repeated <c>body</c> keeps its last value.
    /// </summary>
    public static PostBodyStatus Read(ReadOnlySpan<byte> json, out string? text) {
        text = null;
        bool isString = false;
        try {
            Utf8JsonReader r = new(json);
            if (!r.Read()) {
                return PostBodyStatus.Malformed;
            }

            if (r.TokenType == JsonTokenType.StartObject) {
                while (r.Read() && r.TokenType == JsonTokenType.PropertyName) {
                    bool isBody = r.ValueTextEquals("body"u8);
                    r.Read();
                    if (isBody) {
                        isString = r.TokenType == JsonTokenType.String;
                        text = isString ? r.GetString() : null;
                    }

                    // Moves past an object or array value; a no-op on a scalar.
                    r.Skip();
                }
            } else {
                r.Skip();
            }

            // Anything after the first value is an error; Read throws on it.
            if (r.Read()) {
                return PostBodyStatus.Malformed;
            }
        } catch (JsonException) {
            text = null;
            return PostBodyStatus.Malformed;
        } catch (InvalidOperationException) {
            // GetString on a string that is not valid UTF-8.
            text = null;
            return PostBodyStatus.Malformed;
        }

        if (!isString) {
            text = null;
            return PostBodyStatus.Required;
        }

        text = text!.Trim();
        if (text.Length == 0) {
            text = null;
            return PostBodyStatus.Required;
        }

        // A string of at most 500 UTF-16 units has at most 500 code points; only longer ones need counting.
        if (text.Length > MaxCharacters && CodePoints(text) > MaxCharacters) {
            text = null;
            return PostBodyStatus.TooLong;
        }

        return PostBodyStatus.Ok;
    }

    /// <summary>Number of Unicode code points in <paramref name="s"/>; a lone surrogate counts as one.</summary>
    private static int CodePoints(string s) {
        int n = 0;
        foreach (Rune _ in s.EnumerateRunes()) {
            n++;
        }

        return n;
    }
}
