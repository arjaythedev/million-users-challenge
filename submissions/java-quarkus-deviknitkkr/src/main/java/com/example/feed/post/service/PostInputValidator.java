package com.example.feed.post.service;

import com.example.feed.shared.error.ApiException;
import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import jakarta.enterprise.context.ApplicationScoped;

/** Invoked after authentication so invalid writes retain the required error precedence. */
@ApplicationScoped
public class PostInputValidator {
    private static final int MAX_BODY_LENGTH = 500;
    private final ObjectMapper json;

    public PostInputValidator(ObjectMapper json) {
        this.json = json;
    }

    public long postId(String rawId) {
        try {
            long id = Long.parseLong(rawId);
            if (id > 0) {
                return id;
            }
        } catch (NumberFormatException ignored) {
            // All invalid forms share the same public error response.
        }
        throw new ApiException(400, "invalid post id");
    }

    public String body(String request) {
        JsonNode document = parse(request);
        JsonNode bodyNode = document == null ? null : document.get("body");
        String body = bodyNode != null && bodyNode.isTextual() ? bodyNode.textValue().strip() : "";
        if (body.isEmpty()) {
            throw new ApiException(400, "body is required");
        }
        if (body.length() > MAX_BODY_LENGTH) {
            throw new ApiException(400, "body must be at most 500 characters");
        }
        return body;
    }

    private JsonNode parse(String request) {
        if (request == null || request.isBlank()) {
            return null;
        }
        try {
            return json.readTree(request);
        } catch (JsonProcessingException exception) {
            throw new ApiException(400, "malformed JSON body");
        }
    }
}
