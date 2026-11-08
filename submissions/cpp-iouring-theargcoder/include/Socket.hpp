#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <unordered_set>
#include <vector>

#include <liburing.h>

#include "include/Defines.hpp"

namespace HttpServer
{
  class Socket
  {
  public:
    // Single io_uring consumer: accepts, receives, sends and recycles buffers.
    void rx_reciver(std::atomic<bool> &running) noexcept;

  private:
    bool ring_initialized{ false };

    // One listener per challenge port.
    std::array<int, 2> listen_fds{ { -1, -1 } };
    std::array<bool, 2> accept_rearm_pending{ { false, false } };

    io_uring iouring{};

    std::vector<io_packet_desc> rx_ring;
    std::vector<io_packet_desc> tx_ring;

    // RX descriptors returned by HTTP workers and waiting to be re-provided.
    std::vector<io_packet_desc *> fill_ring;

    // TX descriptors currently free and available to HTTP workers.
    std::vector<io_packet_desc *> completion_ring;

    // TX descriptors filled by HTTP workers and waiting to be sent.
    std::vector<io_packet_desc *> tx_pending_ring;

    // A multishot receive that ended with ENOBUFS (or with a final successful
    // CQE) is re-armed only after an RX buffer has been provided again.
    std::vector<int> pending_recv_fds;
    std::size_t pending_recv_head{ 0 };
    bool retry_pending_recv{ false };

    char *rx_umem{};
    char *tx_umem{};

    alignas(std::hardware_destructive_interference_size) std::atomic<bool> rx_rel_occupied{ false };
    alignas(std::hardware_destructive_interference_size) std::atomic<bool> tx_get_occupied{ false };
    alignas(std::hardware_destructive_interference_size) std::atomic<bool> tx_rel_occupied{ false };

    alignas(std::hardware_destructive_interference_size) std::atomic<uint32_t> fill_head{ 0 };
    alignas(std::hardware_destructive_interference_size) std::atomic<uint32_t> fill_tail{ 0 };

    alignas(std::hardware_destructive_interference_size) std::atomic<uint32_t> completion_head{ 0 };
    alignas(std::hardware_destructive_interference_size) std::atomic<uint32_t> completion_tail{ 0 };

    alignas(std::hardware_destructive_interference_size) std::atomic<uint32_t> tx_send_head{ 0 };
    alignas(std::hardware_destructive_interference_size) std::atomic<uint32_t> tx_send_tail{ 0 };

    void cleanup() noexcept;

    [[nodiscard]] int create_listener(uint16_t port);
    [[nodiscard]] bool arm_accept(std::size_t listener_index) noexcept;
    [[nodiscard]] bool add_recv_request(int client_fd) noexcept;
    void queue_pending_recv(int client_fd, bool retry_when_sq_available) noexcept;
    void recycle_tx_packet(io_packet_desc *desc) noexcept;

    void close_connection(int fd) noexcept;

  public:
    Socket();
    ~Socket() noexcept;

    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    Socket(Socket &&) = delete;
    Socket &operator=(Socket &&) = delete;

    io_packet_desc *get_tx_packet_ptr() noexcept;
    io_packet_desc *wait_for_tx_packet() noexcept;

    bool release_rx_packet(io_packet_desc *desc) noexcept;
    void wait_for_rx_release(io_packet_desc *desc) noexcept;

    bool release_tx_packet(io_packet_desc *desc) noexcept;
    void wait_for_tx_release(io_packet_desc *desc) noexcept;

    [[nodiscard]] __attribute__((always_inline)) char *get_rx_umem_ptr() noexcept
    {
      return this->rx_umem;
    }

    [[nodiscard]] __attribute__((always_inline)) char *get_tx_umem_ptr() noexcept
    {
      return this->tx_umem;
    }
  };

} // namespace HttpServer
