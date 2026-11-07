#pragma once

#include <cstdint>

namespace feed
{
enum class Tag : unsigned char
{
    Client,
    Listener,
    Pg
};

struct Tagged
{
    Tag tag;
    unsigned char gen = 0;
    bool poll_live = false;
    uint32_t watch_events = 0;
    int watch_fd = -1;

    explicit Tagged(const Tag t) :
        tag(t)
    {
    }
};
}
