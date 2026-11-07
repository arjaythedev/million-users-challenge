<?php

use App\Support\Fail;
use Illuminate\Foundation\Application;
use Illuminate\Foundation\Configuration\Exceptions;
use Illuminate\Foundation\Configuration\Middleware;
use Illuminate\Http\Request;
use Illuminate\Support\Facades\Route;
use Symfony\Component\HttpKernel\Exception\HttpExceptionInterface;

// No web/api middleware groups (no sessions, cookies, CSRF), no global middleware: the routes are
// registered by hand in routes/feed.php and the only middleware is the bearer-token check on the two writes.
return Application::configure(basePath: dirname(__DIR__))
    ->withRouting(using: function () {
        Route::group([], base_path('routes/feed.php'));
    })
    ->withMiddleware(function (Middleware $middleware) {
        $middleware->use([]);
    })
    ->withExceptions(function (Exceptions $exceptions) {
        $exceptions->shouldRenderJsonWhen(fn () => true);
        // Never go through Laravel's logger: requests overlap inside a worker (coroutines), and the container that
        // report() would resolve from can be another request's sandbox, already flushed by Octane. Unexpected errors
        // are written to stderr below; the expected 4xx are not logged at all.
        $exceptions->dontReport(Throwable::class);
        $exceptions->render(function (Fail $e) {
            return Fail::json($e->status, $e->getMessage());
        });
        $exceptions->render(function (HttpExceptionInterface $e, Request $request) {
            return Fail::json($e->getStatusCode() === 404 || $e->getStatusCode() === 405 ? 404 : $e->getStatusCode(),
                $e->getStatusCode() === 404 || $e->getStatusCode() === 405 ? 'not found' : 'internal server error');
        });
        $exceptions->render(function (Throwable $e) {
            error_log(get_class($e).': '.$e->getMessage().' at '.$e->getFile().':'.$e->getLine());

            return Fail::json(500, 'internal server error');
        });
    })->create();
