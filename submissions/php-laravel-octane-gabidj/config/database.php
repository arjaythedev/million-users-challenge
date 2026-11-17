<?php

// DATABASE_URL (postgres://user:pass@host:port/db) is parsed by Laravel's ConfigurationUrlParser.
return [
    'default' => 'pgsql',
    'connections' => [
        'pgsql' => [
            'driver' => 'pgsql',
            'url' => env('DATABASE_URL'),
            'charset' => 'utf8',
            'search_path' => 'public',
            'sslmode' => 'disable',
            'timezone' => 'UTC',
        ],
    ],
    'migrations' => ['table' => 'migrations'],
];
