using Feed.Http;

namespace Feed.Api;

/// <summary>The fixed error responses of SPEC.md, built once. Each body is {"error":"message"}.</summary>
public static class Responses {
    /// <summary>404: unknown path.</summary>
    public static readonly byte[] NotFound = Build(404, "not found"u8);
    /// <summary>400: the id is not a positive integer.</summary>
    public static readonly byte[] InvalidPostId = Build(400, "invalid post id"u8);
    /// <summary>404: the post does not exist.</summary>
    public static readonly byte[] PostNotFound = Build(404, "post not found"u8);
    /// <summary>401: no Authorization header, or it does not start with "Bearer ".</summary>
    public static readonly byte[] MissingBearer = Build(401, "missing bearer token"u8);
    /// <summary>401: bad signature, expired, not HS256 or malformed.</summary>
    public static readonly byte[] InvalidToken = Build(401, "invalid or expired token"u8);
    /// <summary>401: a valid token whose sub or username is unusable.</summary>
    public static readonly byte[] InvalidPayload = Build(401, "invalid token payload"u8);
    /// <summary>400: the request body is not valid JSON.</summary>
    public static readonly byte[] MalformedJson = Build(400, "malformed JSON body"u8);
    /// <summary>400: body is missing, not a string, or empty after trimming.</summary>
    public static readonly byte[] BodyRequired = Build(400, "body is required"u8);
    /// <summary>400: body is longer than 500 characters after trimming.</summary>
    public static readonly byte[] BodyTooLong = Build(400, "body must be at most 500 characters"u8);
    /// <summary>500: anything else.</summary>
    public static readonly byte[] ServerError = Build(500, "internal server error"u8);

    /// <summary>The whole response for <paramref name="status"/> with <paramref name="message"/> as its error.</summary>
    private static byte[] Build(int status, ReadOnlySpan<byte> message) {
        ResponseWriter w = new();
        w.WriteError(message);
        return w.Finish(status).ToArray();
    }
}
