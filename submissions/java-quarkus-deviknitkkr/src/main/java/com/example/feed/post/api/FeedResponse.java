package com.example.feed.post.api;

import com.example.feed.post.model.Post;
import java.util.List;

public record FeedResponse(List<Post> posts) {
}
