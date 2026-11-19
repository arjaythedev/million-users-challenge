package com.example.feed.infrastructure.config;

import io.vertx.core.Vertx;
import io.vertx.pgclient.PgBuilder;
import io.vertx.pgclient.PgConnectOptions;
import io.vertx.sqlclient.PoolOptions;
import io.vertx.mutiny.sqlclient.SqlClient;
import jakarta.annotation.PreDestroy;
import jakarta.enterprise.context.ApplicationScoped;
import jakarta.enterprise.inject.Produces;
import jakarta.inject.Singleton;
import org.eclipse.microprofile.config.inject.ConfigProperty;

/** Pooled client multiplexes independent autocommit queries without holding a connection per request. */
@ApplicationScoped
public class ReactiveClient {
    private final io.vertx.sqlclient.SqlClient client;

    public ReactiveClient(Vertx vertx,
            @ConfigProperty(name = "quarkus.datasource.reactive.url") String url,
            @ConfigProperty(name = "quarkus.datasource.username") String username,
            @ConfigProperty(name = "quarkus.datasource.password", defaultValue = "") String password,
            @ConfigProperty(name = "quarkus.datasource.reactive.max-size") int connections,
            @ConfigProperty(name = "DB_PIPELINE", defaultValue = "8") int pipeline,
            @ConfigProperty(name = "DB_WAIT_QUEUE", defaultValue = "0") int waitQueue) {
        if (pipeline < 1 || pipeline > 16) {
            throw new IllegalArgumentException("DB_PIPELINE must be between 1 and 16");
        }
        if (waitQueue < 0) {
            throw new IllegalArgumentException("DB_WAIT_QUEUE must not be negative");
        }
        if (waitQueue == 0) {
            // Absorbs short stalls (GC, DB hiccup) proportionally to throughput.
            waitQueue = 4096 * Runtime.getRuntime().availableProcessors();
        }
        var connect = PgConnectOptions.fromUri(url).setUser(username).setPassword(password)
                .setCachePreparedStatements(true).setPipeliningLimit(pipeline);
        // Vert.x 4.5 leaves acquisition timers running after queue rejection. Their
        // second promise failure can flood the event loops with exceptions. Bound
        // queued work instead; do not time out/replay potentially committed writes.
        // Without an event-loop size, a pool built at startup puts every connection's
        // I/O on a single event loop; spread them across all loops so DB work scales with cores.
        var pool = new PoolOptions().setMaxSize(connections).setMaxWaitQueueSize(waitQueue)
                .setEventLoopSize(eventLoops(vertx))
                .setConnectionTimeout(0);
        client = PgBuilder.client().with(pool).connectingTo(connect).using(vertx).build();
    }

    private static int eventLoops(Vertx vertx) {
        int count = 0;
        for (var ignored : vertx.nettyEventLoopGroup()) {
            count++;
        }
        return Math.max(1, count);
    }

    @Produces
    @Singleton
    SqlClient client() {
        return SqlClient.newInstance(client);
    }

    @PreDestroy
    void close() {
        client.close().toCompletionStage().toCompletableFuture().join();
    }
}
