package feed;

import feed.net.Io;
import feed.server.Server;

/** Starts the server with settings from the environment ({@link Config#fromEnv}) and runs until killed. */
public final class Main {

    private Main() {
    }

    public static void main(String[] args) throws Exception {
        Config config = Config.fromEnv(System.getenv());
        Server server = Server.start(config);
        Runtime.getRuntime().addShutdownHook(new Thread(server::close, "shutdown"));
        System.err.printf("feed: listening on %s:%d%s with %d %s workers, %d read + %d write Postgres connections,"
                        + " pipeline depth %d, gzip %s%n",
                config.host(), server.port(), server.extraPort() >= 0 ? " and :" + server.extraPort() : "",
                config.threads(), Io.backend(), config.readConns(), config.writeConns(), config.pipelineDepth(),
                server.gzip() + (config.gzipLevel() > 0 ? " from " + config.gzipMin() + " bytes" : ""));
        server.awaitTermination();
    }
}
