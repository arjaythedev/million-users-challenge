#pragma once

#include <cstdint>

#include "net.hpp"
#include "tag.hpp"

namespace feed
{
inline constexpr uint32_t kWatchIn = 1;
inline constexpr uint32_t kWatchOut = 4;

class Watcher
{
  public:
    virtual ~Watcher() = default;

    virtual bool add(Tagged *who, net::Socket fd, uint32_t events) = 0;

    virtual void modify(Tagged *who, net::Socket fd, uint32_t events) = 0;

    virtual void remove(Tagged *who, net::Socket fd) = 0;
};
}
