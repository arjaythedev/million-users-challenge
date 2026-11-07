package com.example.feed.post.model;

import com.example.feed.infrastructure.json.MillisecondInstantSerializer;
import com.fasterxml.jackson.databind.annotation.JsonSerialize;
import java.time.Instant;

/**
 * Serialized as {"id","body","created_at","author","like_count"} (record order + SNAKE_CASE).
 * created_at is a Postgres TIMESTAMPTZ (microseconds), read as an Instant and formatted by Jackson in
 * UTC with millisecond precision, truncating rather than rounding.
 */
public record Post(
        long id,
        String body,
        @JsonSerialize(using = MillisecondInstantSerializer.class)
        Instant createdAt,
        String author,
        int likeCount) {
}
