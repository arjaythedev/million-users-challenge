<?php

namespace App\Support;

use Illuminate\Http\Response;

class Respond
{
    /** Compact JSON with the key order of the array. Swoole compresses the body when the client sends Accept-Encoding: gzip. */
    public static function json(array $data, int $status = 200): Response
    {
        return new Response(
            json_encode($data, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE | JSON_THROW_ON_ERROR),
            $status,
            ['Content-Type' => 'application/json'],
        );
    }
}
