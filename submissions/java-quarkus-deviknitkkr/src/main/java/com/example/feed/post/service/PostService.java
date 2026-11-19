package com.example.feed.post.service;

import com.example.feed.auth.AuthUser;
import com.example.feed.post.model.Post;
import com.example.feed.post.persistence.PostRepository;
import io.vertx.pgclient.PgException;
import com.example.feed.shared.error.ApiException;
import jakarta.enterprise.context.ApplicationScoped;
import io.smallrye.mutiny.Uni;
import java.util.List;

@ApplicationScoped
public class PostService {
    private static final String FOREIGN_KEY_VIOLATION = "23503";

    public record LikeResult(long postId, boolean inserted) {
    }

    private final PostRepository repository;
    private final PostInputValidator input;

    public PostService(PostRepository repository, PostInputValidator input) {
        this.repository = repository;
        this.input = input;
    }

    public Uni<List<Post>> feed() {
        return repository.feed();
    }

    public Uni<Post> get(String rawId) {
        return repository.get(input.postId(rawId)).map(post -> {
            if (post == null) {
                throw new ApiException(404, "post not found");
            }
            return post;
        });
    }

    public Uni<Post> create(AuthUser user, String request) {
        return repository.create(user, input.body(request));
    }

    public Uni<LikeResult> like(AuthUser user, String rawId) {
        long postId = input.postId(rawId);
        return repository.like(user.id(), postId).map(inserted -> new LikeResult(postId, inserted))
                .onFailure(PgException.class).transform(exception ->
                        FOREIGN_KEY_VIOLATION.equals(((PgException) exception).getSqlState())
                                ? new ApiException(404, "post not found") : exception);
    }
}
