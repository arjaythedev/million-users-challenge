package com.example.feed;

import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.MessageDigest;
import java.util.Base64;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

import io.vertx.core.json.JsonObject;

final class Jwt {
    record Claims(long userId, String username) {
    }

    private static final Base64.Decoder B64 = Base64.getUrlDecoder();

    private final ThreadLocal<Mac> macs;

    Jwt(String secret) {
        SecretKeySpec key = new SecretKeySpec(secret.getBytes(StandardCharsets.UTF_8), "HmacSHA256");
        this.macs = ThreadLocal.withInitial(() -> {
            try {
                Mac mac = Mac.getInstance("HmacSHA256");
                mac.init(key);
                return mac;
            } catch (GeneralSecurityException e) {
                throw new IllegalStateException(e);
            }
        });
    }

    Claims authenticate(String authorization) {
        if (authorization == null || !authorization.startsWith("Bearer ")) {
            throw ApiError.MISSING_TOKEN;
        }
        String token = authorization.substring(7);
        int dot1 = token.indexOf('.');
        int dot2 = dot1 < 0 ? -1 : token.indexOf('.', dot1 + 1);
        if (dot1 <= 0 || dot2 <= dot1 + 1 || dot2 == token.length() - 1 || token.indexOf('.', dot2 + 1) >= 0) {
            throw ApiError.INVALID_TOKEN;
        }
        JsonObject payload;
        try {
            JsonObject header = new JsonObject(new String(B64.decode(token.substring(0, dot1)), StandardCharsets.UTF_8));
            if (!"HS256".equals(header.getValue("alg"))) {
                throw ApiError.INVALID_TOKEN;
            }
            Mac mac = macs.get();
            byte[] expected = mac.doFinal(token.substring(0, dot2).getBytes(StandardCharsets.US_ASCII));
            if (!MessageDigest.isEqual(expected, B64.decode(token.substring(dot2 + 1)))) {
                throw ApiError.INVALID_TOKEN;
            }
            payload = new JsonObject(new String(B64.decode(token.substring(dot1 + 1, dot2)), StandardCharsets.UTF_8));
        } catch (ApiError e) {
            throw e;
        } catch (RuntimeException e) {
            throw ApiError.INVALID_TOKEN;
        }
        if (!(payload.getValue("exp") instanceof Number exp) || exp.doubleValue() <= System.currentTimeMillis() / 1000.0) {
            throw ApiError.INVALID_TOKEN;
        }
        if (!(payload.getValue("username") instanceof String username)
                || !(payload.getValue("sub") instanceof String sub)) {
            throw ApiError.INVALID_PAYLOAD;
        }
        long userId = Ids.parsePositive(sub);
        if (userId <= 0) {
            throw ApiError.INVALID_PAYLOAD;
        }
        return new Claims(userId, username);
    }
}
