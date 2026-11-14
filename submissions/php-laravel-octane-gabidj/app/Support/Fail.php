<?php

namespace App\Support;

use Illuminate\Http\Response;
use RuntimeException;

/** An API error: status + the {"error": "..."} body from SPEC.md. */
class Fail extends RuntimeException
{
    public function __construct(public int $status, string $message)
    {
        parent::__construct($message);
    }

    public static function json(int $status, string $message): Response
    {
        return Respond::json(['error' => $message], $status);
    }
}
