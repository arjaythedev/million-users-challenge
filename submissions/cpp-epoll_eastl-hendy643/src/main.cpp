#include <charconv>
#include <cstdio>
#include <cstdlib>

#include "server.hpp"

namespace
{
feed::String env(const char *name, const char *fallback = nullptr)
{
    const char *value = std::getenv(name);
    return {value ? value : (fallback ? fallback : "")};
}

int env_int(const char *name, const char *fallback)
{
    const feed::String text = env(name, fallback);
    int value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}
} // namespace

int main()
{
    feed::ServerConfig config;
    config.database_url = env("DATABASE_URL");
    config.jwt_secret = env("JWT_SECRET");
    if (config.database_url.empty() || config.jwt_secret.empty())
    {
        std::fprintf(stderr, "DATABASE_URL and JWT_SECRET are required\n");
        return 2;
    }
    config.host = env("HOST", "0.0.0.0");
    config.port = env_int("PORT", "8080");
    config.extra_port = env_int("EXTRA_PORT", "8081");
    return feed::run_server(config);
}
