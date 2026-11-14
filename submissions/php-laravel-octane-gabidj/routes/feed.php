<?php

use App\Http\Controllers\FeedController;
use App\Http\Middleware\Authenticate;
use Illuminate\Support\Facades\Route;

Route::get('/health', [FeedController::class, 'health']);
Route::get('/feed', [FeedController::class, 'feed']);
Route::get('/posts/{id}', [FeedController::class, 'show']);
Route::post('/posts', [FeedController::class, 'store'])->middleware(Authenticate::class);
Route::post('/posts/{id}/like', [FeedController::class, 'like'])->middleware(Authenticate::class);
