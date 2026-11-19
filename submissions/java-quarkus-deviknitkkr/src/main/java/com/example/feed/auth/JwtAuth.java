package com.example.feed.auth;

import com.example.feed.shared.error.ApiException;
import com.auth0.jwt.JWT;
import com.auth0.jwt.JWTVerifier;
import com.auth0.jwt.algorithms.Algorithm;
import com.auth0.jwt.exceptions.JWTVerificationException;
import com.auth0.jwt.interfaces.DecodedJWT;
import jakarta.enterprise.context.ApplicationScoped;
import org.eclipse.microprofile.config.inject.ConfigProperty;

@ApplicationScoped
public class JwtAuth {
    private static final String BEARER_PREFIX = "Bearer ";
    private final JWTVerifier verifier;

    public JwtAuth(@ConfigProperty(name = "JWT_SECRET") String secret) {
        verifier = JWT.require(Algorithm.HMAC256(secret)).build();
    }

    public AuthUser authenticate(String header) {
        if (header == null || !header.startsWith(BEARER_PREFIX)) {
            throw new ApiException(401, "missing bearer token");
        }
        DecodedJWT jwt;
        try {
            jwt = verifier.verify(header.substring(BEARER_PREFIX.length()).trim());
        } catch (JWTVerificationException ex) {
            throw new ApiException(401, "invalid or expired token");
        }
        String subject = jwt.getClaim("sub").asString();
        String username = jwt.getClaim("username").asString();
        long id = parseSubject(subject);
        if (id <= 0 || username == null) {
            throw new ApiException(401, "invalid token payload");
        }
        return new AuthUser(id, username);
    }

    private static long parseSubject(String subject) {
        if (subject == null || subject.isEmpty()) {
            return 0;
        }
        for (int i = 0; i < subject.length(); i++) {
            char character = subject.charAt(i);
            if (character < '0' || character > '9') {
                return 0;
            }
        }
        try {
            return Long.parseLong(subject);
        } catch (NumberFormatException exception) {
            return 0;
        }
    }
}
