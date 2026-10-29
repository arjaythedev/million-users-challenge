package com.example.feed;

import java.util.concurrent.TimeUnit;

import jakarta.enterprise.context.ApplicationScoped;
import jakarta.enterprise.event.Observes;

import org.eclipse.microprofile.config.inject.ConfigProperty;
import org.jboss.logging.Logger;

import io.vertx.core.MultiMap;
import io.vertx.core.buffer.Buffer;
import io.vertx.core.http.HttpHeaders;
import io.vertx.ext.web.Router;
import io.vertx.ext.web.RoutingContext;
import io.vertx.ext.web.handler.BodyHandler;
import io.vertx.pgclient.PgException;
import io.vertx.sqlclient.Pool;
import io.vertx.sqlclient.Row;
import io.vertx.sqlclient.RowIterator;
import io.vertx.sqlclient.Tuple;

@ApplicationScoped
public class FeedRoutes {
    private static final Logger LOG = Logger.getLogger(FeedRoutes.class);
    private static final CharSequence APPLICATION_JSON = HttpHeaders.createOptimized("application/json");
    private static final long STARTED = System.nanoTime();
    private static final int GZIP_MIN_BYTES = 256;

    private final Pool pool;
    private final Jwt jwt;

    FeedRoutes(Pool pool, @ConfigProperty(name = "JWT_SECRET") String jwtSecret) {
        this.pool = pool;
        this.jwt = new Jwt(jwtSecret);
    }

    void routes(@Observes Router router) {
        BodyHandler body = BodyHandler.create(false).setBodyLimit(16 * 1024);
        router.get("/health").handler(this::health);
        router.get("/feed").handler(this::feed);
        router.get("/posts/:id").handler(this::getPost);
        router.post("/posts").handler(body).handler(this::createPost);
        router.post("/posts/:id/like").handler(body).handler(this::like);
        router.route().last().handler(ctx -> send(ctx, ApiError.NOT_FOUND));
        router.route().failureHandler(ctx -> {
            if (ctx.failure() != null) {
                LOG.error("request failed", ctx.failure());
            }
            send(ctx, ApiError.INTERNAL);
        });
    }

    private void health(RoutingContext ctx) {
        pool.preparedQuery(Sql.HEALTH).execute(ar -> {
            if (ar.succeeded()) {
                send(ctx, 200, Json.health(TimeUnit.NANOSECONDS.toSeconds(System.nanoTime() - STARTED)));
            } else {
                String error = ar.cause().getMessage();
                send(ctx, 503, Json.degraded(error != null ? error : ar.cause().toString()));
            }
        });
    }

    private void feed(RoutingContext ctx) {
        pool.preparedQuery(Sql.FEED).execute(ar -> {
            if (ar.succeeded()) {
                send(ctx, 200, Json.feed(ar.result()));
            } else {
                fail(ctx, ar.cause());
            }
        });
    }

    private void getPost(RoutingContext ctx) {
        long id;
        try {
            id = postId(ctx);
        } catch (ApiError e) {
            send(ctx, e);
            return;
        }
        pool.preparedQuery(Sql.POST_BY_ID).execute(Tuple.of(id), ar -> {
            if (ar.failed()) {
                fail(ctx, ar.cause());
                return;
            }
            RowIterator<Row> rows = ar.result().iterator();
            if (rows.hasNext()) {
                send(ctx, 200, Json.post(rows.next()));
            } else {
                send(ctx, ApiError.POST_NOT_FOUND);
            }
        });
    }

    private void createPost(RoutingContext ctx) {
        Jwt.Claims user;
        String text;
        try {
            user = jwt.authenticate(ctx.request().getHeader(HttpHeaders.AUTHORIZATION));
            text = Json.postBody(ctx.body().buffer());
        } catch (ApiError e) {
            send(ctx, e);
            return;
        }
        pool.preparedQuery(Sql.INSERT_POST).execute(Tuple.of(user.userId(), text), ar -> {
            if (ar.failed()) {
                fail(ctx, ar.cause());
                return;
            }
            Row r = ar.result().iterator().next();
            send(ctx, 201, Json.post(r.getLong(0), text, r.getOffsetDateTime(1), user.username(), 0));
        });
    }

    private void like(RoutingContext ctx) {
        Jwt.Claims user;
        long id;
        try {
            user = jwt.authenticate(ctx.request().getHeader(HttpHeaders.AUTHORIZATION));
            id = postId(ctx);
        } catch (ApiError e) {
            send(ctx, e);
            return;
        }
        pool.preparedQuery(Sql.INSERT_LIKE).execute(Tuple.of(user.userId(), id), ar -> {
            if (ar.succeeded()) {
                boolean inserted = ar.result().rowCount() == 1;
                send(ctx, inserted ? 201 : 200, Json.like(!inserted, id));
            } else if (ar.cause() instanceof PgException pg && "23503".equals(pg.getSqlState())) {
                send(ctx, ApiError.POST_NOT_FOUND);
            } else {
                fail(ctx, ar.cause());
            }
        });
    }

    private static long postId(RoutingContext ctx) {
        long id = Ids.parsePositive(ctx.pathParam("id"));
        if (id <= 0) {
            throw ApiError.INVALID_POST_ID;
        }
        return id;
    }

    private static void fail(RoutingContext ctx, Throwable cause) {
        LOG.error("query failed", cause);
        send(ctx, ApiError.INTERNAL);
    }

    private static void send(RoutingContext ctx, ApiError error) {
        send(ctx, error.status, Buffer.buffer(error.body));
    }

    private static void send(RoutingContext ctx, int status, Buffer body) {
        if (ctx.response().ended()) {
            return;
        }
        ctx.response().setStatusCode(status);
        MultiMap headers = ctx.response().headers();
        headers.set(HttpHeaders.CONTENT_TYPE, APPLICATION_JSON);
        if (body.length() >= GZIP_MIN_BYTES) {
            headers.remove(HttpHeaders.CONTENT_ENCODING);
        }
        ctx.response().end(body);
    }
}
