#pragma once

#include <cstdint>

#include "types.hpp"

namespace feed
{

enum class AuthStatus
{
    Ok,
    MissingBearer,
    InvalidToken,
    InvalidPayload
};

struct Claims
{
    int64_t user_id = 0;
    String username;
};

struct AuthResult
{
    AuthStatus status = AuthStatus::MissingBearer;
    Claims claims;
};

AuthResult authenticate(View authorization, View secret, int64_t now);

}
