<?php

namespace App\Http\Controllers;

use App\Support\Db;
use App\Support\Fail;
use App\Support\Respond;
use Illuminate\Http\Request;
use Illuminate\Http\Response;
use JsonException;
use PDOException;

class FeedController
{
    private const POST_SELECT = 'SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
  FROM posts p JOIN users u ON u.id = p.user_id';

    private static ?int $started = null;

    /** "2025-12-31 23:59:19.409123+00" (session time zone is UTC) -> "2025-12-31T23:59:19.409Z", truncated to ms. */
    private static function ts(string $t): string
    {
        // Postgres drops trailing zeros of the fraction (".16", or none at all), so take only its digits
        $ms = ($t[19] ?? '') === '.' ? substr($t, 20, min(3, strspn($t, '0123456789', 20))) : '';

        return substr($t, 0, 10).'T'.substr($t, 11, 8).'.'.str_pad($ms, 3, '0').'Z';
    }

    private static function post(array $r): array
    {
        return ['id' => (int) $r['id'], 'body' => $r['body'], 'created_at' => self::ts($r['created_at']),
            'author' => $r['username'], 'like_count' => (int) $r['like_count']];
    }

    private static function id(string $raw): int
    {
        if (! preg_match('/^[0-9]{1,15}$/D', $raw) || (int) $raw <= 0) {
            throw new Fail(400, 'invalid post id');
        }

        return (int) $raw;
    }

    public function health(): Response
    {
        self::$started ??= time();
        try {
            Db::rows('SELECT 1');
        } catch (PDOException $e) {
            return Respond::json(['status' => 'degraded', 'db' => 'unreachable', 'error' => $e->getMessage()], 503);
        }

        return Respond::json(['status' => 'ok', 'db' => 'ok', 'uptime_s' => time() - self::$started]);
    }

    public function feed(): Response
    {
        $rows = Db::rows(self::POST_SELECT.' ORDER BY p.created_at DESC, p.id DESC LIMIT 20');

        return Respond::json(['posts' => array_map([self::class, 'post'], $rows)]);
    }

    public function show(string $id): Response
    {
        $rows = Db::rows(self::POST_SELECT.' WHERE p.id = ?', [self::id($id)]);
        if (! $rows) {
            throw new Fail(404, 'post not found');
        }

        return Respond::json(['post' => self::post($rows[0])]);
    }

    public function store(Request $request): Response
    {
        try {
            $data = json_decode($request->getContent(), true, 512, JSON_THROW_ON_ERROR);
        } catch (JsonException) {
            throw new Fail(400, 'malformed JSON body');
        }
        $text = is_array($data) ? ($data['body'] ?? null) : null;
        if (! is_string($text)) {
            throw new Fail(400, 'body is required');
        }
        // like JS trim(): Unicode whitespace, BOM included
        $text = preg_replace('/^[\p{Z}\s\x{FEFF}]+|[\p{Z}\s\x{FEFF}]+$/u', '', $text);
        if ($text === '' || $text === null) {
            throw new Fail(400, 'body is required');
        }
        if (mb_strlen($text) > 500) {
            throw new Fail(400, 'body must be at most 500 characters');
        }
        $uid = $request->attributes->get('uid');
        $row = Db::rows('INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at', [$uid, $text])[0];

        return Respond::json(['post' => ['id' => (int) $row['id'], 'body' => $text, 'created_at' => self::ts($row['created_at']),
            'author' => $request->attributes->get('username'), 'like_count' => 0]], 201);
    }

    public function like(Request $request, string $id): Response
    {
        $id = self::id($id);
        try {
            $n = Db::exec('INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING',
                [$request->attributes->get('uid'), $id]);
        } catch (PDOException $e) {
            if ($e->getCode() === '23503') {
                throw new Fail(404, 'post not found');
            }
            throw $e;
        }

        return Respond::json(['liked' => true, 'already_liked' => $n !== 1, 'post_id' => $id], $n === 1 ? 201 : 200);
    }
}
