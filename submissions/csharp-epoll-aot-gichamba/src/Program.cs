using Feed.Api;
using Feed.Http;
using Feed.Pg;

namespace Feed;

/// <summary>Process entry point: reads the settings, starts the loops with their Postgres connections, serves HTTP.</summary>
public static class Program {
    /// <summary>
    /// Starts the server and serves until the process is stopped. A startup failure, such as a bad setting or a port in
    /// use, prints one line and returns 1.
    /// </summary>
    public static int Main() {
        // The first console write duplicates fds 1 and 2, which needs free descriptors. Write now: under load every
        // descriptor can be a client connection, and a first write then throws "Too many open files".
        Console.Error.WriteLine("feed: starting");

        Settings settings;
        PgConnectionString connection;
        try {
            settings = Config.Load(Environment.GetEnvironmentVariable);
            connection = PgConnectionString.Parse(settings.DatabaseUrl);
        } catch (Exception ex) {
            Console.Error.WriteLine("startup failed: " + ex.Message);
            return 1;
        }

        Stats stats = new(FeedApi.Endpoints);
        (int reads, int writes) = Config.PgPerLoop(settings);

        // Each loop owns its Postgres connections and takes clients once they are open. If Postgres is not up within
        // 10 s, the loops take clients anyway, and a loop's requests get 500 and /health 503 until its connections open.
        try {
            HttpServer.Start(settings, loop => {
                LoopDb db = new(loop, connection, reads, writes, settings.PgPack, stats);
                db.Start();
                return new FeedApi(settings, db.Reads, db.Writes, stats);
            }, stats);
        } catch (Exception ex) {
            Console.Error.WriteLine("startup failed: " + ex.Message);
            return 1;
        }

        Console.Error.WriteLine($"feed: serving on {settings.Host}:{settings.Port} and :{settings.ExtraPort} with {settings.HttpLoops} loops, "
            + $"{reads} read and {writes} write connections each");
        stats.StartReporter(settings.StatsSeconds);
        Thread.Sleep(Timeout.Infinite);
        return 0;
    }
}
