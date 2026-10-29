#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace feed::net
{
using Socket = int;
constexpr Socket kInvalid = -1;

inline void close_socket(const Socket s)
{
    close(s);
}
} // namespace feed::net
