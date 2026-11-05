package challenge;

import io.micronaut.core.annotation.Nullable;
import io.micronaut.http.HttpRequest;
import io.micronaut.http.HttpResponse;
import io.micronaut.http.HttpStatus;
import io.micronaut.http.MediaType;
import io.micronaut.http.annotation.Body;
import io.micronaut.http.annotation.Controller;
import io.micronaut.http.annotation.Error;
import io.micronaut.http.annotation.Get;
import io.micronaut.http.annotation.Post;
import io.micronaut.http.server.netty.NettyHttpRequest;
import io.micronaut.json.JsonMapper;
import io.micronaut.json.tree.JsonNode;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.function.Supplier;

@Controller
public final class FeedController {
    private final Database db;
    private final JwtVerifier jwt;
    private final JsonMapper json;
    private final long started = System.nanoTime();

    public FeedController(Database db, JwtVerifier jwt, JsonMapper json) {
        this.db = db;
        this.jwt = jwt;
        this.json = json;
    }

    private static HttpResponse<?> response(int status, Object body) {
        return HttpResponse.status(HttpStatus.valueOf(status)).body(body).contentType(MediaType.APPLICATION_JSON_TYPE);
    }

    private static HttpResponse<?> directResponse(Object body) {
        var response = HttpResponse.ok(body).contentType(MediaType.APPLICATION_JSON_TYPE);
        response.bodyWriter(DirectBodyWriter.INSTANCE);
        return response;
    }

    static HttpResponse<?> failure(Throwable error) {
        while (error instanceof CompletionException && error.getCause() != null) error = error.getCause();
        if (error instanceof ApiException api) return response(api.status, new Responses.Error(api.getMessage()));
        if (error instanceof io.micronaut.http.exceptions.HttpStatusException status && status.getStatus() == HttpStatus.NOT_FOUND)
            return response(404, new Responses.Error("not found"));
        return response(500, new Responses.Error("internal server error"));
    }

    private static CompletableFuture<HttpResponse<?>> handle(Supplier<CompletableFuture<HttpResponse<?>>> action) {
        try {
            return action.get().exceptionally(FeedController::failure);
        } catch (Exception e) {
            return CompletableFuture.completedFuture(failure(e));
        }
    }

    @Get("/health")
    public CompletableFuture<HttpResponse<?>> health() {
        try {
            return db.health().<HttpResponse<?>>handle((ok, error) -> {
                if (error != null || !Boolean.TRUE.equals(ok))
                    return response(503, new Responses.Degraded("degraded", "unreachable", "internal server error"));
                return response(200, new Responses.Health("ok", "ok", (System.nanoTime() - started) / 1_000_000_000));
            });
        } catch (Exception e) {
            return CompletableFuture.completedFuture(response(503, new Responses.Degraded("degraded", "unreachable", "internal server error")));
        }
    }

    @Get("/feed")
    public CompletableFuture<HttpResponse<?>> feed(HttpRequest<?> request) {
        // Encode on this connection's loop, avoiding a second handoff.
        var eventLoop = ((NettyHttpRequest<?>) request).getChannelHandlerContext().channel().eventLoop();
        return handle(() -> db.feed().thenApplyAsync(posts -> directResponse(new Responses.Feed(posts)), eventLoop));
    }

    @Get("/posts/{id}")
    public CompletableFuture<HttpResponse<?>> post(String id) {
        return handle(() -> db.find(id(id)).thenApply(post -> directResponse(new Responses.Single(post))));
    }

    @Post(value = "/posts", consumes = MediaType.ALL)
    public CompletableFuture<HttpResponse<?>> create(HttpRequest<?> request, @Nullable @Body String raw) {
        return handle(() -> {
            var user = jwt.verify(request.getHeaders().get("Authorization"));
            String body = body(raw);
            return db.create(user, body).thenApply(post -> response(201, new Responses.Single(post)));
        });
    }

    @Post(value = "/posts/{id}/like", consumes = MediaType.ALL)
    public CompletableFuture<HttpResponse<?>> like(String id, HttpRequest<?> request) {
        return handle(() -> {
            var user = jwt.verify(request.getHeaders().get("Authorization"));
            long postId = id(id);
            return db.like(user.id(), postId).thenApply(already ->
                response(already ? 200 : 201, new Responses.Like(true, already, postId)));
        });
    }

    static long id(String value) {
        try {
            for (int i = 0; i < value.length(); i++)
                if (value.charAt(i) < '0' || value.charAt(i) > '9') throw new NumberFormatException();
            long id = Long.parseLong(value);
            if (id > 0) return id;
        } catch (NumberFormatException ignored) {}
        throw new ApiException(400, "invalid post id");
    }

    String body(String raw) {
        JsonNode node;
        try {
            node = json.readValue(raw == null ? "" : raw, JsonNode.class);
        } catch (Exception e) {
            throw new ApiException(400, "malformed JSON body");
        }
        JsonNode body = node != null && node.isObject() ? node.get("body") : null;
        if (body == null || !body.isString()) throw new ApiException(400, "body is required");
        String value = body.getStringValue().strip();
        if (value.isEmpty()) throw new ApiException(400, "body is required");
        if (value.codePointCount(0, value.length()) > 500)
            throw new ApiException(400, "body must be at most 500 characters");
        return value;
    }


    @Error(global = true)
    public HttpResponse<?> error(Throwable error) {
        return failure(error);
    }
}
