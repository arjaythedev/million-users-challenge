package com.example.feed.post.api;

import com.example.feed.auth.AuthUser;
import com.example.feed.auth.JwtAuth;
import com.example.feed.post.service.PostService;
import io.smallrye.mutiny.Uni;
import jakarta.ws.rs.GET;
import jakarta.ws.rs.HeaderParam;
import jakarta.ws.rs.POST;
import jakarta.ws.rs.Path;
import jakarta.ws.rs.PathParam;
import jakarta.ws.rs.Produces;
import jakarta.ws.rs.core.MediaType;
import jakarta.ws.rs.core.Response;

@Path("/")
@Produces(MediaType.APPLICATION_JSON)
public class PostController {
    private final PostService posts;
    private final JwtAuth auth;

    public PostController(PostService posts, JwtAuth auth) {
        this.posts = posts;
        this.auth = auth;
    }

    @GET
    @Path("feed")
    public Uni<FeedResponse> feed() {
        return posts.feed().map(FeedResponse::new);
    }

    @GET
    @Path("posts/{id}")
    public Uni<PostResponse> get(@PathParam("id") String id) {
        return posts.get(id).map(PostResponse::new);
    }

    @POST
    @Path("posts")
    public Uni<Response> create(@HeaderParam("Authorization") String authorization, String request) {
        AuthUser user = auth.authenticate(authorization);
        return posts.create(user, request).map(post ->
                Response.status(Response.Status.CREATED).entity(new PostResponse(post)).build());
    }

    @POST
    @Path("posts/{id}/like")
    public Uni<Response> like(@HeaderParam("Authorization") String authorization, @PathParam("id") String id) {
        AuthUser user = auth.authenticate(authorization);
        return posts.like(user, id).map(result -> {
            Response.Status status = result.inserted() ? Response.Status.CREATED : Response.Status.OK;
            return Response.status(status)
                    .entity(new LikeResponse(true, !result.inserted(), result.postId()))
                    .build();
        });
    }
}
