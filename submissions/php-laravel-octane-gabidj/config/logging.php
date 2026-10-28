<?php

// Errors go to stderr (journald); nothing is written per request.
return [
    'default' => 'stderr',
    'channels' => [
        'stderr' => [
            'driver' => 'monolog',
            'level' => 'error',
            'handler' => Monolog\Handler\StreamHandler::class,
            'with' => ['stream' => 'php://stderr'],
            'formatter' => Monolog\Formatter\LineFormatter::class,
        ],
    ],
];
