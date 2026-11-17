<?php

// opcache.preload: runs once when the Swoole server process starts; the forked workers share the linked classes.
// Only what a request touches: container, routing, http, events, pipeline, the pgsql connection, the app, and the
// parts of Symfony that Laravel's request/response/router are built on. An explicit list (not "everything under
// Illuminate\"), because a class whose parent lives in a package we don't ship would be a fatal error at start.
require __DIR__.'/vendor/autoload.php';

$prefixes = [
    'Illuminate\\Support\\', 'Illuminate\\Container\\', 'Illuminate\\Contracts\\', 'Illuminate\\Http\\',
    'Illuminate\\Routing\\', 'Illuminate\\Pipeline\\', 'Illuminate\\Events\\', 'Illuminate\\Config\\',
    'Illuminate\\Collections\\', 'Illuminate\\Macroable\\', 'Illuminate\\Conditionable\\',
    'Illuminate\\Database\\Connection', 'Illuminate\\Database\\PostgresConnection', 'Illuminate\\Database\\Connectors\\',
    'Illuminate\\Database\\DatabaseManager', 'Illuminate\\Database\\Query\\', 'Illuminate\\Database\\Grammar',
    'Illuminate\\Foundation\\Http\\', 'Illuminate\\Foundation\\Application', 'Illuminate\\Foundation\\Exceptions\\',
    'Symfony\\Component\\HttpFoundation\\', 'Symfony\\Component\\HttpKernel\\HttpKernel',
    'Symfony\\Component\\HttpKernel\\Exception\\', 'Symfony\\Component\\Routing\\Route',
    'Symfony\\Component\\Routing\\Matcher\\', 'Symfony\\Component\\Routing\\Exception\\',
    'Symfony\\Component\\Routing\\RequestContext', 'Laravel\\Octane\\', 'App\\',
];
$skip = '/Testing|Fake|Test|Console|Command|Session|DependencyInjection|DataCollector|Profiler|HttpCache|Bundle|Debug|Loader|Whoops|Inspector|Browser|ExpressionLanguage|Attribute|Mime|Cache\\\\Adapter/';

foreach (require __DIR__.'/vendor/composer/autoload_classmap.php' as $class => $file) {
    foreach ($prefixes as $p) {
        if (str_starts_with($class, $p) && ! preg_match($skip, $class)) {
            class_exists($class) || interface_exists($class) || trait_exists($class);
            break;
        }
    }
}
