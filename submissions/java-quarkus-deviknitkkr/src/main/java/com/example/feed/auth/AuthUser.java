package com.example.feed.auth;

/** The authenticated caller, verified for each write request by {@link JwtAuth}. */
public record AuthUser(long id, String username) {
}
