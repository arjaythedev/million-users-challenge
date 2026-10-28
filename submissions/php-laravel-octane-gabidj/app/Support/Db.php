<?php

namespace App\Support;

use Illuminate\Support\Facades\DB as Laravel;
use PDO;
use PDOException;
use PDOStatement;

/**
 * Laravel's pgsql connection (config/database.php, one per Octane worker, kept open between requests) with the
 * prepared statements cached on it: a PDOStatement is a server-side prepared plan (Parse once, then Bind/Execute),
 * not data. If the connection drops, the connection and the statements are rebuilt and the call is retried once.
 */
class Db
{
    /** @var array<string, PDOStatement> */
    private static array $stmts = [];

    private static ?PDO $pdo = null;

    private static function pdo(): PDO
    {
        return self::$pdo ??= Laravel::connection('pgsql')->getPdo();
    }

    private static function prepare(string $sql): PDOStatement
    {
        return self::$stmts[$sql] ??= self::pdo()->prepare($sql);
    }

    /** @return list<array<string, mixed>> */
    public static function rows(string $sql, array $params = []): array
    {
        return self::run($sql, $params)->fetchAll(PDO::FETCH_ASSOC);
    }

    /** @return int rows affected */
    public static function exec(string $sql, array $params = []): int
    {
        return self::run($sql, $params)->rowCount();
    }

    private static function run(string $sql, array $params, bool $retry = true): PDOStatement
    {
        try {
            $st = self::prepare($sql);
            $st->execute($params);

            return $st;
        } catch (PDOException $e) {
            if ($retry && self::lostConnection($e)) {
                self::$stmts = [];
                self::$pdo = null;
                Laravel::purge('pgsql');

                return self::run($sql, $params, false);
            }
            throw $e;
        }
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
