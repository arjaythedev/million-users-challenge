<?php

// Not used: Octane serves the app. Kept so `php artisan serve` works for debugging.
use Illuminate\Http\Request;

define('LARAVEL_START', microtime(true));

require __DIR__.'/../vendor/autoload.php';

(require_once __DIR__.'/../bootstrap/app.php')->handleRequest(Request::capture());
