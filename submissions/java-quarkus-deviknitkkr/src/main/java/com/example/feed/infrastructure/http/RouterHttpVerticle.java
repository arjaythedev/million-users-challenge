package com.example.feed.infrastructure.http;

import io.vertx.core.AbstractVerticle;
import io.vertx.core.Promise;
import io.vertx.core.http.HttpServerOptions;
import io.vertx.ext.web.Router;

/** Vert.x shares the socket across instances while each instance owns its server context. */
final class RouterHttpVerticle extends AbstractVerticle {
    private static final int IDLE_TIMEOUT_SECONDS = 90;
    private static final int ACCEPT_BACKLOG = 8192;

    private final String host;
    private final int port;
    private final Router router;

    RouterHttpVerticle(String host, int port, Router router) {
        this.host = host;
        this.port = port;
        this.router = router;
    }

    @Override
    public void start(Promise<Void> started) {
        HttpServerOptions options = ServerOptions.apply(new HttpServerOptions())
                .setHost(host)
                .setPort(port)
                .setIdleTimeout(IDLE_TIMEOUT_SECONDS)
                .setAcceptBacklog(ACCEPT_BACKLOG)
                .setCompressionSupported(true);

        vertx.createHttpServer(options).requestHandler(router).listen().onComplete(result -> {
            if (result.succeeded()) {
                started.complete();
            } else {
                started.fail(result.cause());
            }
        });
    }
}
