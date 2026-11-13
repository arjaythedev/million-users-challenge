using System.Globalization;

namespace Feed;

/// <summary>A startup setting is missing or has a value we do not accept. The message names the variable.</summary>
public sealed class ConfigException : Exception {
    /// <summary>Creates the exception for <paramref name="variable"/> with a reason.</summary>
    public ConfigException(string variable, string reason)
        : base($"{variable}: {reason}") {
    }
}

/// <summary>Reads <see cref="Settings"/> from environment variables, once at startup.</summary>
public static class Config {
    /// <summary>The most Postgres connections the process may hold (rule 5).</summary>
    public const int MaxPgConnections = 512;

    /// <summary>
    /// Builds the settings from <paramref name="getenv"/>. A variable that is unset or empty keeps the default from
    /// <see cref="Settings"/>. A value out of range, or more Postgres connections than the rules allow, throws
    /// <see cref="ConfigException"/> naming the variable.
    /// </summary>
    public static Settings Load(Func<string, string?> getenv) {
        string? Get(string name) {
            string? v = getenv(name);
            return string.IsNullOrEmpty(v) ? null : v;
        }

        Settings s = new() {
            DatabaseUrl = Get("DATABASE_URL") ?? throw new ConfigException("DATABASE_URL", "is required"),
            JwtSecret = Get("JWT_SECRET") ?? throw new ConfigException("JWT_SECRET", "is required"),
        };

        if (Get("HOST") is { } host) {
            s = s with { Host = host };
        }

        s = Int(Get, "PORT", 1, 65535, s, (x, v) => x with { Port = v });
        s = Int(Get, "EXTRA_PORT", 0, 65535, s, (x, v) => x with { ExtraPort = v });
        s = Int(Get, "HTTP_LOOPS", 1, 256, s, (x, v) => x with { HttpLoops = v });
        s = Int(Get, "PG_READ", 1, MaxPgConnections, s, (x, v) => x with { PgRead = v });
        s = Int(Get, "PG_WRITE", 1, MaxPgConnections, s, (x, v) => x with { PgWrite = v });
        s = Int(Get, "PG_PACK", 1, 1024, s, (x, v) => x with { PgPack = v });
        s = Int(Get, "COALESCE_MIN", 0, 1024, s, (x, v) => x with { CoalesceMin = v });
        s = Int(Get, "COALESCE_US", 0, 10_000, s, (x, v) => x with { CoalesceUs = v });
        s = Int(Get, "GZIP_MIN", 0, 1 << 20, s, (x, v) => x with { GzipMin = v });
        s = Int(Get, "STATS_SECONDS", 1, 3600, s, (x, v) => x with { StatsSeconds = v });
        if (s.ExtraPort == s.Port) {
            throw new ConfigException("EXTRA_PORT", $"is the same as PORT ({s.Port})");
        }

        if (s.PgRead + s.PgWrite > MaxPgConnections) {
            throw new ConfigException("PG_READ", $"PG_READ ({s.PgRead}) plus PG_WRITE ({s.PgWrite}) is {s.PgRead + s.PgWrite} connections, more than the {MaxPgConnections} the rules allow");
        }

        // Each loop opens at least one connection of each kind, so many loops can open more than the totals ask for.
        (int reads, int writes) = PgPerLoop(s);
        if (s.HttpLoops * (reads + writes) > MaxPgConnections) {
            throw new ConfigException("HTTP_LOOPS", $"{s.HttpLoops} loops with {reads} read and {writes} write connections each open {s.HttpLoops * (reads + writes)}, more than the {MaxPgConnections} the rules allow");
        }

        return s;
    }

    /// <summary>
    /// The read and write connections each loop opens: its share of <see cref="Settings.PgRead"/> and
    /// <see cref="Settings.PgWrite"/>, rounded down, and at least one of each.
    /// </summary>
    public static (int Reads, int Writes) PgPerLoop(Settings s) =>
        (Math.Max(1, s.PgRead / s.HttpLoops), Math.Max(1, s.PgWrite / s.HttpLoops));

    /// <summary>Applies integer variable <paramref name="name"/> when set. It must be plain digits from <paramref name="min"/> to <paramref name="max"/>.</summary>
    private static Settings Int(Func<string, string?> get, string name, int min, int max, Settings s, Func<Settings, int, Settings> apply) {
        string? text = get(name);
        if (text is null) {
            return s;
        }

        if (!int.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out int v) || v < min || v > max) {
            throw new ConfigException(name, $"unknown value '{text}', expected an integer from {min} to {max}");
        }

        return apply(s, v);
    }
}
