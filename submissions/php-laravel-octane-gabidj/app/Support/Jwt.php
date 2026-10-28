<?php

namespace App\Support;

/** HS256 verification, done on every request (no cache of verified tokens). */
class Jwt
{
    private static string $secret = '';

    private static function b64(string $raw): string
    {
        return rtrim(strtr(base64_encode($raw), '+/', '-_'), '=');
    }

    /** @return array<string, mixed>|null the payload, or null when the token is malformed, not HS256, badly signed or expired */
    public static function verify(string $token): ?array
    {
        $parts = explode('.', $token);
        if (count($parts) !== 3) {
            return null;
        }
        [$h, $p, $s] = $parts;
        $header = json_decode(base64_decode(strtr($h, '-_', '+/')) ?: '', true);
        if (! is_array($header) || ($header['alg'] ?? null) !== 'HS256') {
            return null;
        }
        $secret = self::$secret ?: (self::$secret = (string) getenv('JWT_SECRET'));
        if (! hash_equals(self::b64(hash_hmac('sha256', "$h.$p", $secret, true)), $s)) {
            return null;
        }
        $payload = json_decode(base64_decode(strtr($p, '-_', '+/')) ?: '', true);
        if (! is_array($payload)) {
            return null;
        }
        if (isset($payload['exp']) && (! is_numeric($payload['exp']) || $payload['exp'] <= time())) {
            return null;
        }

        return $payload;
    }
}
