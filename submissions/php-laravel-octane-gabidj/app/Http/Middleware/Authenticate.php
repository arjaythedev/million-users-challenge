<?php

namespace App\Http\Middleware;

use App\Support\Fail;
use App\Support\Jwt;
use Closure;
use Illuminate\Http\Request;

/** Runs before the id check in the controller: POST /posts/abc/like without a token is a 401. */
class Authenticate
{
    public function handle(Request $request, Closure $next)
    {
        $h = $request->headers->get('Authorization');
        if (! is_string($h) || ! str_starts_with($h, 'Bearer ')) {
            throw new Fail(401, 'missing bearer token');
        }
        $payload = Jwt::verify(substr($h, 7));
        if ($payload === null) {
            throw new Fail(401, 'invalid or expired token');
        }
        $sub = $payload['sub'] ?? null;
        if (! is_string($sub) || ! preg_match('/^[0-9]{1,18}$/D', $sub) || (int) $sub <= 0 || ! is_string($payload['username'] ?? null)) {
            throw new Fail(401, 'invalid token payload');
        }
        $request->attributes->set('uid', (int) $sub);
        $request->attributes->set('username', $payload['username']);

        return $next($request);
    }
}
