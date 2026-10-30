namespace Feed;

/// <summary>
/// Every runtime setting, read once at startup from the environment by <see cref="Config.Load"/>. The box sets
/// DATABASE_URL, JWT_SECRET, HOST, PORT and EXTRA_PORT. The rest are tuning values; set one in the environment only to
/// experiment.
/// </summary>
public sealed record Settings {
    /// <summary>Postgres URL (DATABASE_URL), required. <see cref="Pg.PgConnectionString.Parse"/> reads it.</summary>
    public required string DatabaseUrl { get; init; }
    /// <summary>Secret behind the JWT signature (JWT_SECRET), required. Its UTF-8 bytes are the signing key.</summary>
    public required string JwtSecret { get; init; }
    /// <summary>IPv4 address the HTTP listeners bind to (HOST).</summary>
    public string Host { get; init; } = "0.0.0.0";
    /// <summary>First TCP port of the HTTP listeners (PORT), 1 to 65535.</summary>
    public int Port { get; init; } = 80;
    /// <summary>Second TCP port serving the same API (EXTRA_PORT), 1 to 65535, or 0 for none.</summary>
    public int ExtraPort { get; init; } = 81;

    /// <summary>Number of HTTP event loops (HTTP_LOOPS, 1 to 256). Each listens on every port. Defaults to one per core.</summary>
    public int HttpLoops { get; init; } = Environment.ProcessorCount;

    /// <summary>Postgres connections for reads and health checks in all (PG_READ, 1 to 512), split over the loops; each loop gets at least one.</summary>
    public int PgRead { get; init; } = 64;
    /// <summary>Postgres connections for inserts in all (PG_WRITE, 1 to 512), split over the loops; each loop gets at least one.</summary>
    public int PgWrite { get; init; } = 32;
    /// <summary>Most reads a loop pass puts behind one Sync on one connection before it takes the next (PG_PACK, 1 to 1024).</summary>
    public int PgPack { get; init; } = 32;

    /// <summary>A loop pass with fewer events than this is followed by a wait (COALESCE_MIN, 0 to 1024).</summary>
    public int CoalesceMin { get; init; } = 32;
    /// <summary>
    /// The wait after a light pass, in microseconds (COALESCE_US, 0 to 10000); 0 turns it off. On the LAN rig, turning it
    /// off doubled the CPU per request, and 2 ms used about 10% less CPU than 1 ms for a p99 of about 7 ms instead of 4.
    /// </summary>
    public int CoalesceUs { get; init; } = 2000;

    /// <summary>
    /// Smallest body in bytes that is gzipped when the client accepts it (GZIP_MIN, 0 to 1048576); 0, the default,
    /// turns gzip off. On the VM rig, sending plain bodies used 11% less CPU per request, extra packets included.
    /// </summary>
    public int GzipMin { get; init; }

    /// <summary>Seconds between stats lines on stderr (STATS_SECONDS, 1 to 3600).</summary>
    public int StatsSeconds { get; init; } = 10;
}
