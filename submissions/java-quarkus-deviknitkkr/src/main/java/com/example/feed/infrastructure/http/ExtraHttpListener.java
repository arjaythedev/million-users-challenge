package com.example.feed.infrastructure.http;

import io.quarkus.runtime.ShutdownEvent;
import io.quarkus.runtime.StartupEvent;
import io.vertx.core.Vertx;
import io.vertx.core.DeploymentOptions;
import io.vertx.ext.web.Router;
import jakarta.enterprise.context.ApplicationScoped;
import jakarta.enterprise.event.Observes;
import org.eclipse.microprofile.config.inject.ConfigProperty;
import org.jboss.logging.Logger;

/** A second listener uses the same Quarkus router, REST controllers and database pool. */
@ApplicationScoped
public class ExtraHttpListener {
    private static final Logger LOG = Logger.getLogger(ExtraHttpListener.class);
    private final Vertx vertx;
    private final String host;
    private final int port;
    private Router router;
    private String deployment;

    public ExtraHttpListener(Vertx vertx, @ConfigProperty(name = "HOST", defaultValue = "127.0.0.1") String host,
            @ConfigProperty(name = "EXTRA_PORT", defaultValue = "0") int port) {
        this.vertx = vertx;
        this.host = host;
        this.port = port;
    }

    void router(@Observes Router router) {
        this.router = router;
    }

    void start(@Observes StartupEvent event) throws Exception {
        if (port <= 0) {
            return;
        }
        int instances = eventLoopCount();
        LOG.infof("Available CPUs: %d; extra-port event-loop instances: %d",
                Runtime.getRuntime().availableProcessors(), instances);
        deployment = vertx.deployVerticle(() -> new RouterHttpVerticle(host, port, router),
                        new DeploymentOptions().setInstances(instances))
                .toCompletionStage().toCompletableFuture().get();
    }

    private int eventLoopCount() {
        int count = 0;
        for (var ignored : vertx.nettyEventLoopGroup()) {
            count++;
        }
        return count;
    }

    void stop(@Observes ShutdownEvent event) throws Exception {
        // Undeploy closes the servers owned by each verticle context.
        if (deployment != null) {
            vertx.undeploy(deployment).toCompletionStage().toCompletableFuture().get();
        }
    }
}
