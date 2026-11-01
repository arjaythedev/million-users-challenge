package com.example.feed.shared.error;

public final class ApiException extends RuntimeException {
    private final int status;

    public ApiException(int status, String message) {
        super(message, null, false, false);
        this.status = status;
    }

    public int getStatus() {
        return status;
    }
}
