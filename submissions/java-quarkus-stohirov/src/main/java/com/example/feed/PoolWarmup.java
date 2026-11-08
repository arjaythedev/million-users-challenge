package com.example.feed;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.TimeUnit;

import jakarta.enterprise.context.ApplicationScoped;
import jakarta.enterprise.event.Observes;

import org.eclipse.microprofile.config.inject.ConfigProperty;
import org.jboss.logging.Logger;

import io.quarkus.runtime.StartupEvent;
import io.vertx.core.Future;
import io.vertx.sqlclient.Pool;
import io.vertx.sqlclient.SqlConnection;

@ApplicationScoped
public class PoolWarmup {
    private static final Logger LOG = Logger.getLogger(PoolWarmup.class);

    @ConfigProperty(name = "quarkus.datasource.reactive.max-size")
    int size;

    void warm(@Observes StartupEvent event, Pool pool) {
        List<Future<SqlConnection>> connections = new ArrayList<>(size);
        for (int i = 0; i < size; i++) {
            connections.add(pool.getConnection());
        }
        Future.join(connections).onComplete(done -> connections.forEach(f -> f.onSuccess(SqlConnection::close)));
        try {
            Future.join(connections).toCompletionStage().toCompletableFuture().get(20, TimeUnit.SECONDS);
        } catch (Exception e) {
            LOG.warnf("Postgres pool warm-up incomplete: %s", e.getMessage());
        }
        long opened = connections.stream().filter(Future::succeeded).count();
        LOG.infof("Postgres pool warmed: %d/%d connections open", opened, size);
    }
}
