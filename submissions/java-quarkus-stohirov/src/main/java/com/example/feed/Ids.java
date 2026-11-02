package com.example.feed;

final class Ids {
    static long parsePositive(String s) {
        int n = s.length();
        if (n == 0 || n > 18 || s.charAt(0) == '0') {
            return -1;
        }
        long v = 0;
        for (int i = 0; i < n; i++) {
            char c = s.charAt(i);
            if (c < '0' || c > '9') {
                return -1;
            }
            v = v * 10 + (c - '0');
        }
        return v;
    }

    private Ids() {
    }
}
