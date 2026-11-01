using Microsoft.Net.Http.Headers;

namespace MillionUsers;

internal static class Compression
{
    public static bool AcceptsGzip(HttpContext context)
    {
        // Benchmark/browser common case needs no header parser allocations.
        if (context.Request.Headers.AcceptEncoding == "gzip") return true;
        var wildcard = false;
        try
        {
            foreach (var coding in context.Request.GetTypedHeaders().AcceptEncoding ?? [])
            {
                // An explicit coding always overrides '*', including an explicit q=0.
                if (coding.Value.Equals("gzip", StringComparison.OrdinalIgnoreCase))
                    return coding.Quality.GetValueOrDefault(1) > 0;
                if (coding.Value.Equals("*", StringComparison.Ordinal))
                    wildcard = coding.Quality.GetValueOrDefault(1) > 0;
            }
        }
        catch (FormatException) { return false; }
        return wildcard;
    }
}
