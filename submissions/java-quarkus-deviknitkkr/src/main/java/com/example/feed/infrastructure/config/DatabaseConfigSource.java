package com.example.feed.infrastructure.config;

import java.net.URI;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;
import org.eclipse.microprofile.config.spi.ConfigSource;

/** Translate the challenge's DATABASE_URL into reactive client connection settings. */
public final class DatabaseConfigSource implements ConfigSource {
    private static final int DEFAULT_POSTGRES_PORT = 5432;
    private static final int MAX_DEFAULT_POOL_SIZE = 512;
    private static final int CONNECTIONS_PER_CPU = 32;
    private static final int MAX_CONNECTIONS = 512;
    private final Map<String, String> properties;

    public DatabaseConfigSource() {
        properties = Map.copyOf(databaseProperties());
    }

    private static Map<String, String> databaseProperties() {
        Map<String, String> properties = new HashMap<>();
        String databaseUrl = System.getenv("DATABASE_URL");
        if (databaseUrl == null) {
            return properties;
        }
        URI uri = URI.create(databaseUrl);
        if (uri.getRawUserInfo() != null) {
            String[] credentials = uri.getRawUserInfo().split(":", 2);
            properties.put("quarkus.datasource.username", decode(credentials[0]));
            if (credentials.length > 1) {
                properties.put("quarkus.datasource.password", decode(credentials[1]));
            }
        }
        int poolSize = Integer.parseInt(System.getenv().getOrDefault("DB_POOL", "0"));
        // Scale with CPUs (4 -> 128, 8 -> 256, 16 -> 512); capped at the challenge's 512 budget.
        if (poolSize <= 0) {
            poolSize = Math.min(MAX_DEFAULT_POOL_SIZE, CONNECTIONS_PER_CPU * Runtime.getRuntime().availableProcessors());
        }
        if (poolSize > MAX_CONNECTIONS) {
            throw new IllegalArgumentException("DB_POOL must not exceed 512 connections");
        }
        properties.put("quarkus.datasource.reactive.url", "postgresql://" + uri.getHost() + ":"
                + (uri.getPort() < 0 ? DEFAULT_POSTGRES_PORT : uri.getPort()) + uri.getRawPath()
                + (uri.getRawQuery() == null ? "" : "?" + uri.getRawQuery()));
        properties.put("quarkus.datasource.reactive.max-size", Integer.toString(poolSize));
        return properties;
    }

    private static String decode(String value) {
        return URLDecoder.decode(value, StandardCharsets.UTF_8);
    }

    @Override
    public Map<String, String> getProperties() {
        return properties;
    }

    @Override
    public Set<String> getPropertyNames() {
        return properties.keySet();
    }

    @Override
    public String getValue(String name) {
        return properties.get(name);
    }

    @Override
    public String getName() {
        return "challenge-database-url";
    }

    @Override
    public int getOrdinal() {
        return 275;
    }
}
