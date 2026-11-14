<?php

use Laravel\Octane\Events\WorkerErrorOccurred;
use Laravel\Octane\Events\WorkerStarting;
use Laravel\Octane\Events\WorkerStopping;
use Laravel\Octane\Listeners\CloseMonologHandlers;
use Laravel\Octane\Listeners\EnsureUploadedFilesAreValid;
use Laravel\Octane\Listeners\EnsureUploadedFilesCanBeMoved;
use Laravel\Octane\Listeners\ReportException;
use Laravel\Octane\Listeners\StopWorkerIfNecessary;

// Everything not set here (event listeners, warm/flush lists) is Octane's own default (config/octane.php of the package).
return [
    'server' => 'swoole',
    // start.sh runs one Octane server per port (Octane listens on one), each with its own state file
    'state_file' => env('OCTANE_STATE_FILE', storage_path('logs/octane-server-state.json')),
    'https' => false,
    // Octane's default RequestReceived/OperationTerminated listeners give every request's sandbox copy of the app to the
    // shared HTTP kernel, router, DB manager... and Worker::handle() flushes that copy when the request ends. That is
    // fine for one request at a time per worker, but here requests overlap (coroutines wait on Postgres): a request that
    // resumes would find the kernel pointing at another request's already flushed container ("Target class [events]
    // does not exist"). This API keeps no per-request state in the container, so those listeners are dropped and the
    // kernel/router keep the base app. Only the worker start/stop/error listeners of Octane's default are kept.
    'listeners' => [
        WorkerStarting::class => [EnsureUploadedFilesAreValid::class, EnsureUploadedFilesCanBeMoved::class],
        WorkerErrorOccurred::class => [ReportException::class, StopWorkerIfNecessary::class],
        WorkerStopping::class => [CloseMonologHandlers::class],
    ],
    'serve_static_files' => false,   // an API: skip the two stat() calls on public/<path> Octane makes per request
    'max_execution_time' => 0,   // no per-request timer-table write
    'garbage' => 50,
    'tables' => [],
    'cache' => ['rows' => 1, 'bytes' => 1000],
    'swoole' => [
        // PHP flags of the Swoole server process: opcache on for the CLI, files never re-stat'ed
        'php_options' => ['-d', 'opcache.enable_cli=1', '-d', 'opcache.validate_timestamps=0',
            '-d', 'opcache.memory_consumption=128', '-d', 'memory_limit=512M',
            // tracing JIT: the workers run the same few code paths for hours
            '-d', 'opcache.jit=tracing', '-d', 'opcache.jit_buffer_size=64M',
            // link the framework classes once, in the server process, before the workers fork
            '-d', 'opcache.preload='.base_path('preload.php'),
            ...(function_exists('posix_geteuid') && posix_geteuid() === 0 ? ['-d', 'opcache.preload_user=root'] : [])],
        'options' => [
            // every request runs in its own coroutine; pdo_pgsql yields while it waits for Postgres (App\Support\Db),
            // so one worker serves many requests at once (Octane's default is enable_coroutine = false)
            'enable_coroutine' => true,
            'reactor_num' => (int) env('SWOOLE_REACTORS', 2),
            'backlog' => 65535,
            'max_conn' => 1048576,
            'open_tcp_nodelay' => true,
            'max_request' => (int) env('MAX_REQUESTS', 100000),
            'http_compression' => true,
            'http_compression_level' => 1,
            // no heartbeat_* options: idle keep-alive connections stay open until the client closes them
        ],
    ],
];
