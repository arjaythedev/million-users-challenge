<?php

declare(strict_types=1);

$ffi = FFI::cdef('
    typedef uint16_t in_port_t;
    typedef uint32_t in_addr_t;
    typedef unsigned short sa_family_t;
    typedef uint32_t socklen_t;
    typedef long ssize_t;

    struct in_addr {
        in_addr_t s_addr;
    };

    struct sockaddr_in {
        sa_family_t sin_family;
        in_port_t sin_port;
        struct in_addr sin_addr;
        unsigned char sin_zero[8];
    };

    typedef union epoll_data {
        void *ptr;
        int fd;
        uint32_t u32;
        uint64_t u64;
    } epoll_data_t;

    struct __attribute__((__packed__)) epoll_event {
        uint32_t events;
        epoll_data_t data;
    };

    int socket(int domain, int type, int protocol);
    int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
    int bind(int sockfd, const struct sockaddr_in *addr, socklen_t addrlen);
    int listen(int sockfd, int backlog);
    int accept4(int sockfd, void *addr, void *addrlen, int flags);
    ssize_t read(int fd, void *buf, size_t count);
    ssize_t write(int fd, const void *buf, size_t count);
    int close(int fd);

    int epoll_create1(int flags);
    int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
    int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
    int *__errno_location(void);
', 'libc.so.6');

const AF_INET = 2;
const SOCK_STREAM = 1;
const SOCK_NONBLOCK = 04000;
const SOCK_CLOEXEC = 02000000;
const SOL_SOCKET = 1;
const SO_REUSEADDR = 2;
const SO_REUSEPORT = 15;
const IPPROTO_TCP = 6;
const TCP_NODELAY = 1;

const EPOLL_CTL_ADD = 1;
const EPOLL_CTL_DEL = 2;
const EPOLLIN = 0x001;
const EPOLLERR = 0x008;
const EPOLLHUP = 0x010;
const EPOLLRDHUP = 0x2000;
const EAGAIN = 11;
const EINTR = 4;

const STATUS_TEXT = [
    200 => '200 OK',
    201 => '201 Created',
    400 => '400 Bad Request',
    401 => '401 Unauthorized',
    404 => '404 Not Found',
    500 => '500 Internal Server Error',
    503 => '503 Service Unavailable',
];

function createListenSocket(FFI $ffi, string $host, int $port): int
{
    $fd = $ffi->socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if ($fd < 0) {
        throw new RuntimeException("socket() failed for port $port");
    }

    $one = $ffi->new('int');
    $one->cdata = 1;
    $ffi->setsockopt($fd, SOL_SOCKET, SO_REUSEADDR, FFI::addr($one), FFI::sizeof($one));
    $ffi->setsockopt($fd, SOL_SOCKET, SO_REUSEPORT, FFI::addr($one), FFI::sizeof($one));
    $ffi->setsockopt($fd, IPPROTO_TCP, TCP_NODELAY, FFI::addr($one), FFI::sizeof($one));

    $addr = $ffi->new('struct sockaddr_in');
    FFI::memset(FFI::addr($addr), 0, FFI::sizeof($addr));
    $addr->sin_family = AF_INET;
    $addr->sin_port = (($port & 0xFF) << 8) | (($port >> 8) & 0xFF);
    $ipLong = ip2long($host === 'localhost' ? '127.0.0.1' : $host);
    $addr->sin_addr->s_addr = $ipLong ? (int) unpack('V', pack('N', $ipLong))[1] : 0;

    if ($ffi->bind($fd, FFI::addr($addr), FFI::sizeof($addr)) < 0) {
        throw new RuntimeException("bind($host:$port) failed");
    }
    if ($ffi->listen($fd, 65535) < 0) {
        throw new RuntimeException("listen($host:$port) failed");
    }
    return $fd;
}

function detectWorkers(): int
{
    if ($env = getenv('WORKERS')) {
        return max(1, (int) $env);
    }
    $cores = 1;
    if (is_readable('/sys/fs/cgroup/cpu.max')) {
        $parts = explode(' ', trim((string) file_get_contents('/sys/fs/cgroup/cpu.max')));
        if (($parts[0] ?? 'max') !== 'max' && isset($parts[1]) && (int) $parts[1] > 0) {
            $cores = max(1, (int) ceil((int) $parts[0] / (int) $parts[1]));
        } else {
            $cores = max(1, (int) shell_exec('nproc 2>/dev/null') ?: 1);
        }
    }
    return min(64, max(4, $cores * 4));
}

$jwtSecret = (string) (getenv('JWT_SECRET') ?: 'million-users-challenge');
$startTime = (int) (getenv('START_TIME') ?: time());
$dbUrl = (string) getenv('DATABASE_URL');
$u = parse_url($dbUrl);
$pgConnStr = sprintf(
    "host=%s port=%d dbname=%s user=%s password=%s options='--timezone=UTC'",
    $u['host'] ?? '127.0.0.1',
    $u['port'] ?? 5432,
    ltrim($u['path'] ?? '/feed', '/'),
    isset($u['user']) ? rawurldecode($u['user']) : 'postgres',
    isset($u['pass']) ? rawurldecode($u['pass']) : ''
);

$dbConn = null;
$dbPrepared = false;

function getDb()
{
    global $dbConn, $dbPrepared, $pgConnStr;
    if ($dbConn === null || pg_connection_status($dbConn) !== PGSQL_CONNECTION_OK) {
        if ($dbConn !== null && @pg_connection_reset($dbConn)) {
            $dbPrepared = false;
        } else {
            $dbConn = @pg_connect($pgConnStr, PGSQL_CONNECT_FORCE_NEW);
            $dbPrepared = false;
            if ($dbConn === false) {
                $dbConn = null;
                throw new RuntimeException('Database connection failed');
            }
        }
    }
    if (!$dbPrepared) {
        @pg_prepare(
            $dbConn,
            'feed',
            'SELECT p.id, p.body, p.created_at, u.username,
                    (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
               FROM posts p JOIN users u ON u.id = p.user_id
              ORDER BY p.created_at DESC, p.id DESC LIMIT 20'
        );
        @pg_prepare(
            $dbConn,
            'get_post',
            'SELECT p.id, p.body, p.created_at, u.username,
                    (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
               FROM posts p JOIN users u ON u.id = p.user_id
              WHERE p.id = $1'
        );
        @pg_prepare(
            $dbConn,
            'insert_post',
            'INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at'
        );
        @pg_prepare(
            $dbConn,
            'insert_like',
            'INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING'
        );
        $dbPrepared = true;
    }
    return $dbConn;
}

function execPrepared(string $stmtName, array $params = [])
{
    global $dbConn, $dbPrepared;
    $db = getDb();
    $res = @pg_execute($db, $stmtName, $params);
    if ($res === false) {
        $dbConn = null;
        $dbPrepared = false;
        $db = getDb();
        $res = @pg_execute($db, $stmtName, $params);
    }
    return $res;
}

function formatIsoUtc(string $pgTs): string
{
    $base = substr($pgTs, 0, 19);
    $base[10] = 'T';
    if (isset($pgTs[19]) && $pgTs[19] === '.') {
        $tzPos = strpos($pgTs, '+', 20) ?: strpos($pgTs, '-', 20) ?: strlen($pgTs);
        $frac = substr($pgTs, 20, $tzPos - 20);
        $ms = substr(str_pad($frac, 3, '0'), 0, 3);
    } else {
        $ms = '000';
    }
    return $base . '.' . $ms . 'Z';
}

function formatPost(array $row): array
{
    return [
        'id' => (int) $row[0],
        'body' => (string) $row[1],
        'created_at' => formatIsoUtc((string) $row[2]),
        'author' => (string) $row[3],
        'like_count' => (int) $row[4],
    ];
}

function b64UrlDecode(string $input): string|false
{
    $rem = strlen($input) % 4;
    if ($rem === 1) {
        return false;
    }
    if ($rem > 0) {
        $input .= str_repeat('=', 4 - $rem);
    }
    return base64_decode(strtr($input, '-_', '+/'), true);
}

function authenticate(string $auth, string $jwtSecret): array|int
{
    if (!str_starts_with($auth, 'Bearer ')) {
        return 1; // missing bearer token
    }
    $token = substr($auth, 7);
    $parts = explode('.', $token);
    if (count($parts) !== 3 || $parts[0] === '' || $parts[1] === '' || $parts[2] === '') {
        return 2; // invalid or expired token
    }
    [$h64, $p64, $s64] = $parts;

    $hRaw = b64UrlDecode($h64);
    $pRaw = b64UrlDecode($p64);
    $sRaw = b64UrlDecode($s64);
    if ($hRaw === false || $pRaw === false || $sRaw === false) {
        return 2;
    }

    $header = json_decode($hRaw, true);
    if (!is_array($header) || ($header['alg'] ?? null) !== 'HS256') {
        return 2;
    }

    if (!hash_equals(hash_hmac('sha256', "$h64.$p64", $jwtSecret, true), $sRaw)) {
        return 2;
    }

    $payload = json_decode($pRaw, true);
    if (!is_array($payload) || !isset($payload['exp']) || (!is_int($payload['exp']) && !is_float($payload['exp'])) || $payload['exp'] <= time()) {
        return 2;
    }

    $sub = $payload['sub'] ?? null;
    $username = $payload['username'] ?? null;
    if (!is_string($sub) || !preg_match('/^[1-9][0-9]*$/', $sub) || !is_string($username)) {
        return 3; // invalid token payload
    }

    return [(int) $sub, $username];
}

function authError(int $code): array
{
    return match ($code) {
        1 => [401, '{"error":"missing bearer token"}'],
        2 => [401, '{"error":"invalid or expired token"}'],
        default => [401, '{"error":"invalid token payload"}'],
    };
}

function handleRequest(string $method, string $path, string $authHdr, string $body): array
{
    global $jwtSecret, $startTime, $dbConn, $dbPrepared;

    try {
        if ($method === 'GET' && $path === '/feed') {
            $res = execPrepared('feed');
            if ($res === false) {
                return [500, '{"error":"internal server error"}'];
            }
            $posts = [];
            while ($row = pg_fetch_row($res)) {
                $posts[] = formatPost($row);
            }
            return [200, json_encode(['posts' => $posts], JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE)];
        }

        if ($method === 'GET' && str_starts_with($path, '/posts/')) {
            $rest = substr($path, 7);
            if (!str_contains($rest, '/')) {
                if (!preg_match('/^[1-9][0-9]*$/', $rest)) {
                    return [400, '{"error":"invalid post id"}'];
                }
                $res = execPrepared('get_post', [(int) $rest]);
                if ($res === false) {
                    return [500, '{"error":"internal server error"}'];
                }
                $row = pg_fetch_row($res);
                if (!$row) {
                    return [404, '{"error":"post not found"}'];
                }
                return [200, json_encode(['post' => formatPost($row)], JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE)];
            }
        }

        if ($method === 'POST' && str_starts_with($path, '/posts/') && str_ends_with($path, '/like')) {
            $idPart = substr($path, 7, -5);
            if ($idPart !== '' && !str_contains($idPart, '/')) {
                $auth = authenticate($authHdr, $jwtSecret);
                if (is_int($auth)) {
                    return authError($auth);
                }
                if (!preg_match('/^[1-9][0-9]*$/', $idPart)) {
                    return [400, '{"error":"invalid post id"}'];
                }
                $postId = (int) $idPart;
                [$userId] = $auth;

                for ($attempt = 0; $attempt < 2; $attempt++) {
                    $db = getDb();
                    @pg_send_execute($db, 'insert_like', [$userId, $postId]);
                    $res = @pg_get_result($db);
                    if ($res !== false && pg_result_status($res) !== PGSQL_FATAL_ERROR) {
                        $inserted = pg_affected_rows($res) === 1;
                        return [
                            $inserted ? 201 : 200,
                            json_encode([
                                'liked' => true,
                                'already_liked' => !$inserted,
                                'post_id' => $postId,
                            ]),
                        ];
                    }
                    if ($res !== false && pg_result_error_field($res, PGSQL_DIAG_SQLSTATE) === '23503') {
                        return [404, '{"error":"post not found"}'];
                    }
                    $dbConn = null;
                    $dbPrepared = false;
                }
                return [500, '{"error":"internal server error"}'];
            }
        }

        if ($method === 'POST' && $path === '/posts') {
            $auth = authenticate($authHdr, $jwtSecret);
            if (is_int($auth)) {
                return authError($auth);
            }
            [$userId, $username] = $auth;

            $data = json_decode($body, true);
            if (!is_array($data) || json_last_error() !== JSON_ERROR_NONE) {
                return [400, '{"error":"malformed JSON body"}'];
            }
            if (!isset($data['body']) || !is_string($data['body'])) {
                return [400, '{"error":"body is required"}'];
            }
            $trimmed = preg_replace('/^\s+|\s+$/u', '', $data['body']) ?? trim($data['body']);
            if ($trimmed === '') {
                return [400, '{"error":"body is required"}'];
            }
            if (strlen(preg_replace('/[\x80-\xBF]/', '', $trimmed)) > 500) {
                return [400, '{"error":"body must be at most 500 characters"}'];
            }

            $res = execPrepared('insert_post', [$userId, $trimmed]);
            if ($res === false) {
                return [500, '{"error":"internal server error"}'];
            }
            $row = pg_fetch_row($res);
            return [
                201,
                json_encode([
                    'post' => [
                        'id' => (int) $row[0],
                        'body' => $trimmed,
                        'created_at' => formatIsoUtc((string) $row[1]),
                        'author' => $username,
                        'like_count' => 0,
                    ],
                ], JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE),
            ];
        }

        if ($method === 'GET' && $path === '/health') {
            try {
                $db = getDb();
                $res = @pg_query($db, 'SELECT 1');
                if ($res === false) {
                    $dbConn = null;
                    $dbPrepared = false;
                    $db = getDb();
                    $res = @pg_query($db, 'SELECT 1');
                    if ($res === false) {
                        throw new RuntimeException(pg_last_error($db));
                    }
                }
                return [200, json_encode([
                    'status' => 'ok',
                    'db' => 'ok',
                    'uptime_s' => max(0, time() - $startTime),
                ])];
            } catch (Throwable $e) {
                return [503, json_encode([
                    'status' => 'degraded',
                    'db' => 'unreachable',
                    'error' => $e->getMessage(),
                ], JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE)];
            }
        }

        return [404, '{"error":"not found"}'];
    } catch (Throwable) {
        return [500, '{"error":"internal server error"}'];
    }
}

$host = (string) (getenv('HOST') ?: '0.0.0.0');
$port1 = (int) (getenv('PORT') ?: 3000);
$port2 = (int) (getenv('EXTRA_PORT') ?: 3001);
$workers = detectWorkers();

$pids = [];
for ($i = 0; $i < $workers; $i++) {
    $pid = pcntl_fork();
    if ($pid < 0) {
        throw new RuntimeException('pcntl_fork() failed');
    }
    if ($pid === 0) {
        // Worker process
        runWorker($ffi, $host, $port1, $port2);
        exit(0);
    }
    $pids[] = $pid;
}

pcntl_async_signals(true);
$shutdown = function () use (&$pids) {
    foreach ($pids as $pid) {
        posix_kill($pid, SIGTERM);
    }
    exit(0);
};
pcntl_signal(SIGTERM, $shutdown);
pcntl_signal(SIGINT, $shutdown);

while (pcntl_wait($status) > 0) {
}

function runWorker(FFI $ffi, string $host, int $port1, int $port2): void
{
    $listen1 = createListenSocket($ffi, $host, $port1);
    $listen2 = createListenSocket($ffi, $host, $port2);

    $epfd = $ffi->epoll_create1(0);
    $ev = $ffi->new('struct epoll_event');

    $ev->events = EPOLLIN;
    $ev->data->fd = $listen1;
    $ffi->epoll_ctl($epfd, EPOLL_CTL_ADD, $listen1, FFI::addr($ev));

    $ev->events = EPOLLIN;
    $ev->data->fd = $listen2;
    $ffi->epoll_ctl($epfd, EPOLL_CTL_ADD, $listen2, FFI::addr($ev));

    $maxEvents = 512;
    $events = $ffi->new("struct epoll_event[$maxEvents]");
    $readBufSize = 16384;
    $readBuf = $ffi->new("char[$readBufSize]");
    $errnoPtr = $ffi->__errno_location();

    $one = $ffi->new('int');
    $one->cdata = 1;
    $oneAddr = FFI::addr($one);
    $oneSize = FFI::sizeof($one);

    $partial = [];

    while (true) {
        $nfds = $ffi->epoll_wait($epfd, $events, $maxEvents, -1);
        if ($nfds <= 0) {
            continue;
        }

        for ($i = 0; $i < $nfds; $i++) {
            $fd = $events[$i]->data->fd;
            $evMask = $events[$i]->events;

            if ($fd === $listen1 || $fd === $listen2) {
                while (true) {
                    $cfd = $ffi->accept4($fd, null, null, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if ($cfd < 0) {
                        break;
                    }
                    $ffi->setsockopt($cfd, IPPROTO_TCP, TCP_NODELAY, $oneAddr, $oneSize);
                    $ev->events = EPOLLIN | EPOLLRDHUP;
                    $ev->data->fd = $cfd;
                    $ffi->epoll_ctl($epfd, EPOLL_CTL_ADD, $cfd, FFI::addr($ev));
                }
                continue;
            }

            if ($evMask & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                unset($partial[$fd]);
                $ffi->close($fd);
                continue;
            }

            $n = $ffi->read($fd, $readBuf, $readBufSize);
            if ($n <= 0) {
                if ($n === 0 || ($errnoPtr[0] !== EAGAIN && $errnoPtr[0] !== EINTR)) {
                    unset($partial[$fd]);
                    $ffi->close($fd);
                }
                continue;
            }

            $raw = ($partial[$fd] ?? '') . FFI::string($readBuf, (int) $n);
            $hdrEnd = strpos($raw, "\r\n\r\n");
            if ($hdrEnd === false) {
                if (strlen($raw) > 32768) {
                    unset($partial[$fd]);
                    $ffi->close($fd);
                } else {
                    $partial[$fd] = $raw;
                }
                continue;
            }

            $hdrBlock = substr($raw, 0, $hdrEnd);
            $firstCrLf = strpos($hdrBlock, "\r\n");
            $reqLine = $firstCrLf === false ? $hdrBlock : substr($hdrBlock, 0, $firstCrLf);
            $sp1 = strpos($reqLine, ' ');
            $sp2 = $sp1 !== false ? strpos($reqLine, ' ', $sp1 + 1) : false;
            if ($sp1 === false || $sp2 === false) {
                unset($partial[$fd]);
                $ffi->close($fd);
                continue;
            }
            $method = substr($reqLine, 0, $sp1);
            $uri = substr($reqLine, $sp1 + 1, $sp2 - $sp1 - 1);
            $qPos = strpos($uri, '?');
            $path = $qPos === false ? $uri : substr($uri, 0, $qPos);

            $contentLen = 0;
            $authHdr = '';
            $acceptGzip = false;
            $closeConn = false;

            if ($firstCrLf !== false) {
                $lines = explode("\r\n", substr($hdrBlock, $firstCrLf + 2));
                foreach ($lines as $line) {
                    $colon = strpos($line, ':');
                    if ($colon === false) {
                        continue;
                    }
                    $k = strtolower(substr($line, 0, $colon));
                    $v = ltrim(substr($line, $colon + 1));
                    if ($k === 'content-length') {
                        $contentLen = (int) $v;
                    } elseif ($k === 'authorization') {
                        $authHdr = $v;
                    } elseif ($k === 'accept-encoding') {
                        $acceptGzip = str_contains($v, 'gzip');
                    } elseif ($k === 'connection') {
                        $closeConn = strtolower($v) === 'close';
                    }
                }
            }

            $totalNeeded = $hdrEnd + 4 + $contentLen;
            if (strlen($raw) < $totalNeeded) {
                if ($totalNeeded > 65536) {
                    unset($partial[$fd]);
                    $ffi->close($fd);
                } else {
                    $partial[$fd] = $raw;
                }
                continue;
            }
            unset($partial[$fd]);

            $reqBody = $contentLen > 0 ? substr($raw, $hdrEnd + 4, $contentLen) : '';
            [$status, $respBody] = handleRequest($method, $path, $authHdr, $reqBody);

            $extraHdr = $closeConn ? "Connection: close\r\n" : "Connection: keep-alive\r\n";
            if ($acceptGzip && strlen($respBody) > 256) {
                $respBody = gzencode($respBody, 1);
                $extraHdr .= "Content-Encoding: gzip\r\n";
            }

            $statusStr = STATUS_TEXT[$status] ?? '500 Internal Server Error';
            $len = strlen($respBody);
            $pkt = "HTTP/1.1 {$statusStr}\r\nContent-Type: application/json\r\nContent-Length: {$len}\r\n{$extraHdr}\r\n{$respBody}";

            $pktLen = strlen($pkt);
            $written = $ffi->write($fd, $pkt, $pktLen);
            if ($written > 0 && $written < $pktLen) {
                $offset = (int) $written;
                while ($offset < $pktLen) {
                    $rem = substr($pkt, $offset);
                    $w = $ffi->write($fd, $rem, strlen($rem));
                    if ($w > 0) {
                        $offset += (int) $w;
                    } elseif ($errnoPtr[0] !== EAGAIN && $errnoPtr[0] !== EINTR) {
                        $closeConn = true;
                        break;
                    }
                }
            }

            if ($closeConn) {
                $ffi->close($fd);
            }
        }
    }
}
