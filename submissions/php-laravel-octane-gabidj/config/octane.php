<?php

// Everything not set here (event listeners, warm/flush lists) is Octane's own default (config/octane.php of the package).
return [
    'server' => 'swoole',
    // start.sh runs one Octane server per port (Octane listens on one), each with its own state file
    'state_file' => env('OCTANE_STATE_FILE', storage_path('logs/octane-server-state.json')),
    'https' => false,
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
            // an idle worker takes the next request (the workers block on Postgres, so a connection must not be
            // pinned to one worker, which is Swoole's default dispatch_mode 2)
            'dispatch_mode' => 3,
            'send_yield' => false,   // Octane's default; Swoole only allows it with dispatch_mode 2/4
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
