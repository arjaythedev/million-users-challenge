package com.example.feed.post.api;

public record LikeResponse(boolean liked, boolean alreadyLiked, long postId) {
}
