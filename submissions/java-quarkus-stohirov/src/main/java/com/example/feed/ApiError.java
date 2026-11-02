package com.example.feed;

import java.nio.charset.StandardCharsets;

final class ApiError extends RuntimeException {
    static final ApiError NOT_FOUND = new ApiError(404, "not found");
    static final ApiError INVALID_POST_ID = new ApiError(400, "invalid post id");
    static final ApiError POST_NOT_FOUND = new ApiError(404, "post not found");
    static final ApiError MISSING_TOKEN = new ApiError(401, "missing bearer token");
    static final ApiError INVALID_TOKEN = new ApiError(401, "invalid or expired token");
    static final ApiError INVALID_PAYLOAD = new ApiError(401, "invalid token payload");
    static final ApiError MALFORMED_JSON = new ApiError(400, "malformed JSON body");
    static final ApiError BODY_REQUIRED = new ApiError(400, "body is required");
    static final ApiError BODY_TOO_LONG = new ApiError(400, "body must be at most 500 characters");
    static final ApiError INTERNAL = new ApiError(500, "internal server error");

    final int status;
    final byte[] body;

    private ApiError(int status, String message) {
        super(message, null, false, false);
        this.status = status;
        this.body = ("{\"error\":\"" + message + "\"}").getBytes(StandardCharsets.UTF_8);
    }
}
