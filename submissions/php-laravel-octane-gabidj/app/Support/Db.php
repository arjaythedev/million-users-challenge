<?php

namespace App\Support;

use Illuminate\Database\Connectors\PostgresConnector;
use Illuminate\Support\Facades\DB as Laravel;
use PDO;
use PDOException;
use RuntimeException;
use Swoole\Coroutine\Channel;
use Swoole\Runtime;
use WeakMap;

/**
 * A per-worker pool of Postgres connections for coroutines.
 *
 * Octane runs every request in its own Swoole coroutine (config/octane.php: enable_coroutine), and
 * SWOOLE_HOOK_PDO_PGSQL makes pdo_pgsql yield to the scheduler while it waits for Postgres. So one worker serves many
 * requests at once: a request borrows a connection for the duration of one statement and the worker runs other requests
 * meanwhile. Connections are built by Laravel's own PostgresConnector from the `pgsql` entry of config/database.php
 * (same DSN, UTC time zone, search_path as the Laravel connection would have).
 *
 * Prepared statements are cached per connection (a PDOStatement is a server-side plan, not data). A connection that
 * dropped is discarded, and the call is retried once on a fresh one.
 */
class Db
{
    private static ?Channel $idle = null;

    private static int $open = 0;

    private static int $max = 8;

    /** @var array<string, mixed> */
    private static array $config = [];

    /** @var WeakMap<PDO, array<string, \PDOStatement>> */
    private static WeakMap $stmts;

    private static function boot(): void
    {
        Runtime::enableCoroutine(SWOOLE_HOOK_PDO_PGSQL);   // only pdo_pgsql yields; nothing else interleaves requests
        self::$max = max(1, (int) (getenv('DB_POOL') ?: 8));
        self::$idle = new Channel(self::$max);
        self::$config = Laravel::connection('pgsql')->getConfig();
        self::$stmts = new WeakMap;
    }

    private static function acquire(): PDO
    {
        if (self::$idle === null) {
            self::boot();
        }
        if (self::$idle->isEmpty() && self::$open < self::$max) {
            self::$open++;   // counted before connecting: connect() yields, other requests must not overshoot the pool
            try {
                return (new PostgresConnector)->connect(self::$config);
            } catch (\Throwable $e) {
                self::$open--;
                throw $e;
            }
        }
        $pdo = self::$idle->pop(10.0);
        if (! $pdo instanceof PDO) {
            throw new RuntimeException('no database connection available');
        }

        return $pdo;
    }

    /** @return list<array<string, mixed>>|int rows of the statement, or the rows affected when $fetch is false */
    private static function run(string $sql, array $params, bool $fetch, bool $retry = true): array|int
    {
        $pdo = self::acquire();
        try {
            $cache = self::$stmts[$pdo] ?? [];
            if (! isset($cache[$sql])) {
                $cache[$sql] = $pdo->prepare($sql);
                self::$stmts[$pdo] = $cache;
            }
            $st = $cache[$sql];
            $st->execute($params);
            $result = $fetch ? $st->fetchAll(PDO::FETCH_ASSOC) : $st->rowCount();
        } catch (PDOException $e) {
            if (self::lostConnection($e)) {
                self::$open--;   // discard it (its statements go with it) and try once more on a fresh connection
                if ($retry) {
                    return self::run($sql, $params, $fetch, false);
                }
            } else {
                self::$idle->push($pdo);   // e.g. a foreign key violation: the connection is fine
            }
            throw $e;
        } catch (\Throwable $e) {
            self::$open--;
            throw $e;
        }
        self::$idle->push($pdo);

        return $result;
    }

    /** @return list<array<string, mixed>> */
    public static function rows(string $sql, array $params = []): array
    {
        return self::run($sql, $params, true);
    }

    /** @return int rows affected */
    public static function exec(string $sql, array $params = []): int
    {
        return self::run($sql, $params, false);
    }

    private static function lostConnection(PDOException $e): bool
    {
        $code = (string) $e->getCode();
        if (str_starts_with($code, '08') || str_starts_with($code, '57P')) {
            return true;
        }

        return (bool) preg_match('/server closed the connection|no connection to the server|terminating connection|could not send data|SSL SYSCALL|connection (is )?(lost|closed)|EOF detected/i', $e->getMessage());
    }
}
