package com.example.feed.infrastructure.http;

import io.quarkus.vertx.http.HttpServerOptionsCustomizer;
import io.vertx.core.http.HttpServerOptions;
import jakarta.enterprise.context.ApplicationScoped;

/**
 * Shared per-connection settings for both listeners. The API serves no WebSockets, so the
 * WebSocket compression handshakers Vert.x otherwise installs on every HTTP/1 pipeline are disabled.
 */
@ApplicationScoped
public class ServerOptions implements HttpServerOptionsCustomizer {
    @Override
    public void customizeHttpServer(HttpServerOptions options) {
        apply(options);
    }

    static HttpServerOptions apply(HttpServerOptions options) {
        return options
                .setPerMessageWebSocketCompressionSupported(false)
                .setPerFrameWebSocketCompressionSupported(false);
    }
}
