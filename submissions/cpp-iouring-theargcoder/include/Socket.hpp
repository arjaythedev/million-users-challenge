
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <new>

#include <liburing.h>

#include "include/Defines.hpp"

namespace HttpServer
{
  class Socket
  {
  private:
    io_uring ring{};
    bool ring_initialized{ false };

    // One listener per challenge port.
    std::array<int, 2> listen_fds{ { -1, -1 } };

    int conn_fd{ -1 }; // Most recently accepted FD.

    io_packet_desc *rx_ring{};
    io_packet_desc *tx_ring{};

    // Allocated and registered by liburing.
    io_uring_buf_ring *buf_ring{};

    char *umem{};

    // Multiple SQ producers, one CQ consumer.
    std::atomic_flag sq_occupied = ATOMIC_FLAG_INIT;

    alignas(std::hardware_destructive_interference_size) std::atomic<uint32_t> rx_consumer{ 0 };
    alignas(std::hardware_destructive_interference_size) std::atomic<uint32_t> rx_producer{ 0 };
    alignas(std::hardware_destructive_interference_size) std::atomic<uint32_t> rx_buffers_available{ 0 };
    alignas(std::hardware_destructive_interference_size) std::atomic<bool> rx_occupied{ false };

    alignas(std::hardware_destructive_interference_size) std::atomic<uint32_t> tx_consumer{ 0 };
    alignas(std::hardware_destructive_interference_size) std::atomic<uint32_t> tx_producer{ 0 };
    alignas(std::hardware_destructive_interference_size) std::atomic<bool> tx_occupied{ false };

    // RX descriptor ownership.
    std::array<std::atomic<bool>, RING_BUFFER_SIZE> rx_slot_released{};
    std::array<std::atomic<bool>, RING_BUFFER_SIZE> rx_close_after{};

    // Multishot feature fallback.
    bool multishot_accept_supported{ true };
    std::atomic<bool> multishot_recv_supported{ true };

    // Connections whose multishot receive stopped because of ENOBUFS.
    // They are rearmed as soon as RX buffers become available.
    std::array<int, PENDING_RECV_QUEUE_SIZE> pending_recv_fds{};

    uint32_t pending_recv_head{ 0 };
    uint32_t pending_recv_tail{ 0 };
    uint32_t pending_recv_count{ 0 };

    std::atomic_flag pending_recv_occupied = ATOMIC_FLAG_INIT;

    void cleanup() noexcept;

    [[nodiscard]] int create_listener(uint16_t port);

    bool queue_accept(uint32_t listener_index) noexcept;
    bool arm_multishot_recv(int fd) noexcept;
    bool queue_send(io_packet_desc &desc) noexcept;

    void advance_tx_consumer() noexcept;

    void defer_recv_rearm(int fd) noexcept;
    void rearm_pending_receives() noexcept;
    void remove_pending_recv_rearm(int fd) noexcept;

    void close_connection(int fd) noexcept;

  public:
    Socket();
    ~Socket() noexcept;

    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    Socket(Socket &&) = delete;
    Socket &operator=(Socket &&) = delete;

    // RX packet lifecycle.
    io_packet_desc *wait_for_rx_packet(std::atomic<bool> &signal) noexcept;
    void release_rx_packet(const io_packet_desc *desc) noexcept;

    // TX packet lifecycle.
    io_packet_desc *get_tx_packet_ptr() noexcept;
    io_packet_desc *wait_for_tx_packet() noexcept;
    bool release_tx_packet(io_packet_desc &desc) noexcept;

    // Connection lifecycle.
    void close_after_pending_sends(int fd) noexcept;

    // UMEM access for HTTP and JSON serialization.
    [[nodiscard]] __attribute__((always_inline)) char *get_umem_ptr() noexcept
    {
      return this->umem;
    }
  };

} // namespace HttpServer
