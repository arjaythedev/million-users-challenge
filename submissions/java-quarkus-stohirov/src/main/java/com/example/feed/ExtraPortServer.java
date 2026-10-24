package com.example.feed;

import java.time.Duration;
import java.util.Optional;
import java.util.concurrent.TimeUnit;

import jakarta.enterprise.context.ApplicationScoped;
import jakarta.enterprise.event.Observes;

import org.eclipse.microprofile.config.inject.ConfigProperty;
import org.jboss.logging.Logger;

import io.quarkus.runtime.StartupEvent;
import io.vertx.core.AbstractVerticle;
import io.vertx.core.DeploymentOptions;
import io.vertx.core.Promise;
import io.vertx.core.Vertx;
import io.vertx.core.http.HttpServerOptions;
import io.vertx.ext.web.Router;

@ApplicationScoped
public class ExtraPortServer {
    private static final Logger LOG = Logger.getLogger(ExtraPortServer.class);

    @ConfigProperty(name = "EXTRA_PORT")
    Optional<Integer> port;
    @ConfigProperty(name = "quarkus.http.host")
    String host;
    @ConfigProperty(name = "quarkus.http.io-threads")
    Optional<Integer> ioThreads;
    @ConfigProperty(name = "quarkus.http.idle-timeout")
    Duration idleTimeout;
    @ConfigProperty(name = "quarkus.http.accept-backlog")
    int acceptBacklog;
    @ConfigProperty(name = "quarkus.http.so-reuse-port")
    boolean reusePort;
    @ConfigProperty(name = "quarkus.http.enable-compression")
    boolean compression;
    @ConfigProperty(name = "quarkus.http.compression-level")
    Optional<Integer> compressionLevel;

    private volatile Router router;

    void captureRouter(@Observes Router router) {
        this.router = router;
    }

    void start(@Observes StartupEvent event, Vertx vertx) {
        if (port.isEmpty()) {
            return;
        }
        HttpServerOptions options = new HttpServerOptions()
                .setHost(host)
                .setPort(port.get())
                .setIdleTimeout((int) idleTimeout.toSeconds())
                .setIdleTimeoutUnit(TimeUnit.SECONDS)
                .setAcceptBacklog(acceptBacklog)
                .setReusePort(reusePort)
                .setCompressionSupported(compression);
        compressionLevel.ifPresent(options::setCompressionLevel);
        int instances = ioThreads.orElse(Runtime.getRuntime().availableProcessors());
        Router r = router;
        vertx.deployVerticle(() -> new AbstractVerticle() {
            @Override
            public void start(Promise<Void> started) {
                vertx.createHttpServer(options).requestHandler(r).listen().<Void> mapEmpty().onComplete(started);
            }
        }, new DeploymentOptions().setInstances(instances))
                .toCompletionStage().toCompletableFuture().join();
        LOG.infof("also listening on http://%s:%d (%d instances, native transport %s)", host, port.get(), instances,
                vertx.isNativeTransportEnabled());
    }
}
