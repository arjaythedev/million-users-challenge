#include "include/Socket.hpp"
#include "include/Defines.hpp"
#include "include/HttpServer.hpp"

#include <arpa/inet.h>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <bit>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <print>
#include <stdexcept>
#include <string>
#include <system_error>

namespace
{
  // user_data layout:
  //   FFFF : multishot accept, low bits = listener index
  //   FFFE : multishot receive, low bits = client fd
  //   FFFD : provide-buffer completion, low bits = RX buffer id
  //   0000 : TX descriptor pointer (normal canonical userspace pointer)
  constexpr uint64_t USER_TAG_MASK = 0xFFFF'0000'0000'0000ULL;
  constexpr uint64_t ACCEPT_TAG = 0xFFFF'0000'0000'0000ULL;
  constexpr uint64_t RECV_TAG = 0xFFFE'0000'0000'0000ULL;
  constexpr uint64_t PROVIDE_TAG = 0xFFFD'0000'0000'0000ULL;
  constexpr uint64_t USER_DATA_MASK = 0x0000'FFFF'FFFF'FFFFULL;

  void lock_flag(std::atomic<bool> &flag) noexcept
  {
    while(flag.exchange(true, std::memory_order_acquire))
    {
      flag.wait(true, std::memory_order_relaxed);
    }
  }

  void unlock_flag(std::atomic<bool> &flag) noexcept
  {
    flag.store(false, std::memory_order_release);
    flag.notify_one();
  }

  [[nodiscard]] io_uring_sqe *get_sqe_or_submit(io_uring &ring) noexcept
  {
    if(io_uring_sqe *sqe = io_uring_get_sqe(&ring))
    {
      return sqe;
    }

    const int submit_result = io_uring_submit(&ring);

    if(submit_result < 0 && submit_result != -EINTR)
    {
      return nullptr;
    }

    return io_uring_get_sqe(&ring);
  }

  [[noreturn]] void throw_io_uring_error(const char *message, int error)
  {
    throw std::runtime_error{ std::string{ message } + ": " + std::strerror(-error) };
  }

  [[noreturn]] void perror_and_throw(const char *message)
  {
    const int error = errno;
    throw std::runtime_error{ std::string{ message } + ": " + std::strerror(error) };
  }

  [[nodiscard]] uint16_t parse_port(const char *value)
  {
    if(value == nullptr || *value == '\0')
    {
      throw std::invalid_argument{ "Socket() - empty listening port" };
    }

    errno = 0;
    char *end = nullptr;
    const long port = std::strtol(value, &end, 10);

    if(errno != 0 || end == value || *end != '\0' || port < 1 || port > 65535)
    {
      throw std::invalid_argument{ std::string{ "Socket() - invalid listening port: " } + value };
    }

    return static_cast<uint16_t>(port);
  }

} // namespace

namespace HttpServer
{
  // ================================================================
  // CONSTRUCTION
  // ================================================================

  Socket::Socket()
  {
    try
    {
      if(!std::has_single_bit(static_cast<uint32_t>(RX_FRAME_COUNT)) || !std::has_single_bit(static_cast<uint32_t>(TX_FRAME_COUNT)))
      {
        throw std::invalid_argument{ "RX_FRAME_COUNT and TX_FRAME_COUNT must be powers of two" };
      }

      // IORING_OP_PROVIDE_BUFFERS takes a signed count and buffer IDs are
      // 16-bit. Keep the count within the supported legacy-provide limit.
      if(RX_FRAME_COUNT == 0 || RX_FRAME_COUNT > USHRT_MAX || TX_FRAME_COUNT == 0 || CHUNK_SIZE == 0 || CHUNK_SIZE > INT_MAX)
      {
        throw std::invalid_argument{ "Invalid frame count or CHUNK_SIZE" };
      }

      const uint64_t rx_required = static_cast<uint64_t>(RX_FRAME_COUNT) * static_cast<uint64_t>(CHUNK_SIZE);
      const uint64_t tx_required = static_cast<uint64_t>(TX_FRAME_COUNT) * static_cast<uint64_t>(CHUNK_SIZE);

      if(rx_required > static_cast<uint64_t>(RX_UMEM_LEN) || tx_required > static_cast<uint64_t>(TX_UMEM_LEN))
      {
        throw std::invalid_argument{ "UMEM length is smaller than frame_count * CHUNK_SIZE" };
      }

      if(RING_BUFFER_SIZE == 0 || RING_BUFFER_SIZE > UINT_MAX / 2U)
      {
        throw std::invalid_argument{ "Invalid RING_BUFFER_SIZE" };
      }

      // Map independently so cleanup() can release RX if TX mmap fails.
      void *rx_memory = mmap(nullptr, static_cast<size_t>(RX_UMEM_LEN), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);

      if(rx_memory == MAP_FAILED)
      {
        this->rx_umem = nullptr;
        perror_and_throw("Socket() - RX mmap failed");
      }

      this->rx_umem = static_cast<char *>(rx_memory);

      void *tx_memory = mmap(nullptr, static_cast<size_t>(TX_UMEM_LEN), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);

      if(tx_memory == MAP_FAILED)
      {
        this->tx_umem = nullptr;
        perror_and_throw("Socket() - TX mmap failed");
      }

      this->tx_umem = static_cast<char *>(tx_memory);

      // Descriptor storage and the independent ownership queues.
      this->rx_ring.resize(RX_FRAME_COUNT);
      this->tx_ring.resize(TX_FRAME_COUNT);
      this->fill_ring.resize(RX_FRAME_COUNT, nullptr);
      this->completion_ring.resize(TX_FRAME_COUNT, nullptr);
      this->tx_pending_ring.resize(TX_FRAME_COUNT, nullptr);
      this->pending_recv_fds.reserve(256);
      this->active_fds.reserve(4096);

      for(uint32_t i = 0; i < RX_FRAME_COUNT; ++i)
      {
        auto &desc = this->rx_ring[i];
        desc.addr = static_cast<uint64_t>(i) * static_cast<uint64_t>(CHUNK_SIZE);
        desc.client_fd = -1;
        desc.len = 0;
        desc.flags = desc_event::SENT;
      }

      for(uint32_t i = 0; i < TX_FRAME_COUNT; ++i)
      {
        auto &desc = this->tx_ring[i];
        desc.addr = static_cast<uint64_t>(i) * static_cast<uint64_t>(CHUNK_SIZE);
        desc.client_fd = -1;
        desc.len = 0;
        desc.flags = desc_event::SENT;

        this->completion_ring[i] = &desc;
      }

      // RX return queue starts empty. TX free queue starts full.
      this->fill_head.store(0, std::memory_order_relaxed);
      this->fill_tail.store(0, std::memory_order_relaxed);
      this->completion_head.store(0, std::memory_order_relaxed);
      this->completion_tail.store(TX_FRAME_COUNT, std::memory_order_relaxed);
      this->tx_send_head.store(0, std::memory_order_relaxed);
      this->tx_send_tail.store(0, std::memory_order_relaxed);

      // Listeners.
      this->listen_fds[0] = this->create_listener(parse_port(PORT));
      this->listen_fds[1] = this->create_listener(parse_port(EXTRA_PORT));

      // io_uring.
      io_uring_params params{};
      params.flags = IORING_SETUP_CQSIZE;
      params.cq_entries = RING_BUFFER_SIZE * 2U;

      const int ring_result = io_uring_queue_init_params(RING_BUFFER_SIZE, &this->iouring, &params);

      if(ring_result < 0)
      {
        throw_io_uring_error("Socket() - io_uring_queue_init_params failed", ring_result);
      }

      this->ring_initialized = true;

      // Provide the RX pool once. TX is sent from tx_umem directly and must
      // NOT be registered as a provided-buffer group.
      {
        io_uring_sqe *sqe = io_uring_get_sqe(&this->iouring);

        if(sqe == nullptr)
        {
          throw std::runtime_error{ "Socket() - no SQE available for initial RX buffer provision" };
        }

        io_uring_prep_provide_buffers(sqe, this->rx_umem, static_cast<int>(CHUNK_SIZE), static_cast<int>(RX_FRAME_COUNT), RX_GID, 0);
        io_uring_sqe_set_data64(sqe, PROVIDE_TAG);

        const int submit_result = io_uring_submit(&this->iouring);

        if(submit_result < 0)
        {
          throw_io_uring_error("Socket() - submitting initial RX buffers failed", submit_result);
        }

        io_uring_cqe *cqe = nullptr;
        const int wait_result = io_uring_wait_cqe(&this->iouring, &cqe);

        if(wait_result < 0)
        {
          throw_io_uring_error("Socket() - waiting for initial RX buffers failed", wait_result);
        }

        const int result = cqe->res;
        io_uring_cqe_seen(&this->iouring, cqe);

        if(result < 0)
        {
          throw_io_uring_error("Socket() - providing initial RX buffers failed", result);
        }
      }

      // Initial multishot accepts. The listener index is encoded in user_data.
      if(!this->arm_accept(0) || !this->arm_accept(1))
      {
        throw std::runtime_error{ "Socket() - failed to arm initial multishot accepts" };
      }

      const int submit_result = io_uring_submit(&this->iouring);

      if(submit_result < 0)
      {
        throw_io_uring_error("Socket() - submitting initial accepts failed", submit_result);
      }

      std::println("Listening on {}:{} and {}:{}", HttpServer::HOST, HttpServer::PORT, HttpServer::HOST, HttpServer::EXTRA_PORT);
    }
    catch(...)
    {
      this->cleanup();
      throw;
    }
  }

  Socket::~Socket() noexcept
  {
    this->cleanup();
  }

  // ================================================================
  // LISTENER / MULTISHOT ARMING
  // ================================================================

  int Socket::create_listener(uint16_t port)
  {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

    if(fd < 0)
    {
      perror_and_throw("socket");
    }

    int enabled = 1;

    if(::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) < 0)
    {
      const int error = errno;
      ::close(fd);
      errno = error;
      perror_and_throw("setsockopt(SO_REUSEADDR)");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if(::inet_pton(AF_INET, HOST, &address.sin_addr) != 1)
    {
      ::close(fd);
      throw std::runtime_error{ std::string{ "Invalid IPv4 HOST: " } + HOST };
    }

    if(::bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0)
    {
      const int error = errno;
      ::close(fd);
      errno = error;
      perror_and_throw("bind");
    }

    if(::listen(fd, 65535) < 0)
    {
      const int error = errno;
      ::close(fd);
      errno = error;
      perror_and_throw("listen");
    }

    return fd;
  }

  bool Socket::arm_accept(std::size_t listener_index) noexcept
  {
    if(listener_index >= this->listen_fds.size() || this->listen_fds[listener_index] < 0)
    {
      return false;
    }

    io_uring_sqe *sqe = get_sqe_or_submit(this->iouring);

    if(sqe == nullptr)
    {
      this->accept_rearm_pending[listener_index] = true;
      return false;
    }

    io_uring_prep_multishot_accept(sqe, this->listen_fds[listener_index], nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    io_uring_sqe_set_data64(sqe, ACCEPT_TAG | static_cast<uint64_t>(listener_index));

    this->accept_rearm_pending[listener_index] = false;
    return true;
  }

  bool Socket::add_recv_request(int client_fd) noexcept
  {
    io_uring_sqe *sqe = get_sqe_or_submit(this->iouring);

    if(sqe == nullptr)
    {
      return false;
    }

    // Multishot recv with provided buffers requires len == 0.
    io_uring_prep_recv_multishot(sqe, client_fd, nullptr, 0, 0);
    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = RX_GID;
    io_uring_sqe_set_data64(sqe, RECV_TAG | static_cast<uint32_t>(client_fd));

    return true;
  }

  void Socket::queue_pending_recv(int client_fd, bool retry_when_sq_available) noexcept
  {
    try
    {
      this->pending_recv_fds.push_back(client_fd);
      this->retry_pending_recv = this->retry_pending_recv || retry_when_sq_available;
    }
    catch(...)
    {
      // Keeping a connection without an armed receive would leak it and stall
      // the peer. Close it if the small pending-rearm queue cannot grow.
      this->close_connection(client_fd);
    }
  }

  // ================================================================
  // TX FREE-FRAME QUEUE
  // ================================================================

  io_packet_desc *Socket::get_tx_packet_ptr() noexcept
  {
    lock_flag(this->tx_get_occupied);

    const uint32_t head = this->completion_head.load(std::memory_order_relaxed);
    const uint32_t tail = this->completion_tail.load(std::memory_order_acquire);

    if(head == tail)
    {
      unlock_flag(this->tx_get_occupied);
      return nullptr;
    }

    const uint32_t index = head & (TX_FRAME_COUNT - 1U);
    io_packet_desc *desc = this->completion_ring[index];

    this->completion_head.store(head + 1U, std::memory_order_release);
    this->completion_head.notify_all();
    unlock_flag(this->tx_get_occupied);

    if(desc != nullptr)
    {
      const auto frame_index = static_cast<uint64_t>(desc - this->tx_ring.data());
      desc->addr = frame_index * static_cast<uint64_t>(CHUNK_SIZE);
      desc->client_fd = -1;
      desc->len = 0;
      desc->flags = desc_event::SENT;
    }

    return desc;
  }

  io_packet_desc *Socket::wait_for_tx_packet() noexcept
  {
    for(;;)
    {
      const uint32_t observed_tail = this->completion_tail.load(std::memory_order_acquire);

      if(io_packet_desc *desc = this->get_tx_packet_ptr())
      {
        return desc;
      }

      this->completion_tail.wait(observed_tail, std::memory_order_acquire);
    }
  }

  void Socket::recycle_tx_packet(io_packet_desc *desc) noexcept
  {
    if(desc == nullptr || this->tx_ring.empty())
    {
      return;
    }

    const auto frame_index_signed = desc - this->tx_ring.data();

    if(frame_index_signed < 0 || static_cast<std::size_t>(frame_index_signed) >= this->tx_ring.size())
    {
      std::println(stderr, "[error] refusing to recycle TX descriptor outside tx_ring");
      return;
    }

    const uint64_t frame_index = static_cast<uint64_t>(frame_index_signed);
    desc->addr = frame_index * static_cast<uint64_t>(CHUNK_SIZE);
    desc->client_fd = -1;
    desc->len = 0;
    desc->flags = desc_event::SENT;

    lock_flag(this->tx_get_occupied);

    const uint32_t head = this->completion_head.load(std::memory_order_acquire);
    const uint32_t tail = this->completion_tail.load(std::memory_order_relaxed);

    if(tail - head >= TX_FRAME_COUNT)
    {
      unlock_flag(this->tx_get_occupied);
      std::println(stderr, "[error] TX free-frame queue overflow (double recycle?)");
      return;
    }

    this->completion_ring[tail & (TX_FRAME_COUNT - 1U)] = desc;
    this->completion_tail.store(tail + 1U, std::memory_order_release);
    unlock_flag(this->tx_get_occupied);
    this->completion_tail.notify_all();
  }

  // ================================================================
  // TX PENDING-SEND QUEUE (MULTI-PRODUCER, SINGLE-CONSUMER)
  // ================================================================

  bool Socket::release_tx_packet(io_packet_desc *desc) noexcept
  {
    if(desc == nullptr)
    {
      return false;
    }

    lock_flag(this->tx_rel_occupied);

    const uint32_t head = this->tx_send_head.load(std::memory_order_acquire);
    const uint32_t tail = this->tx_send_tail.load(std::memory_order_relaxed);

    if(tail - head >= TX_FRAME_COUNT)
    {
      unlock_flag(this->tx_rel_occupied);
      return false;
    }

    this->tx_pending_ring[tail & (TX_FRAME_COUNT - 1U)] = desc;
    this->tx_send_tail.store(tail + 1U, std::memory_order_release);
    unlock_flag(this->tx_rel_occupied);
    this->tx_send_tail.notify_one();

    return true;
  }

  void Socket::wait_for_tx_release(io_packet_desc *desc) noexcept
  {
    for(;;)
    {
      const uint32_t observed_head = this->tx_send_head.load(std::memory_order_acquire);

      if(this->release_tx_packet(desc))
      {
        return;
      }

      // Avoid sleeping on a value that changed between the failed enqueue and
      // the wait (the classic atomic::wait lost-wakeup race).
      if(this->tx_send_head.load(std::memory_order_acquire) == observed_head)
      {
        this->tx_send_head.wait(observed_head, std::memory_order_acquire);
      }
    }
  }

  // ================================================================
  // RX RETURN QUEUE (MULTI-PRODUCER, SINGLE-CONSUMER)
  // ================================================================

  bool Socket::release_rx_packet(io_packet_desc *desc) noexcept
  {
    if(desc == nullptr || desc->addr % CHUNK_SIZE != 0 || desc->addr > static_cast<uint64_t>(RX_UMEM_LEN) - CHUNK_SIZE || desc->addr / CHUNK_SIZE >= RX_FRAME_COUNT)
    {
      return false;
    }

    lock_flag(this->rx_rel_occupied);

    const uint32_t head = this->fill_head.load(std::memory_order_relaxed);
    const uint32_t tail = this->fill_tail.load(std::memory_order_acquire);

    if(head - tail >= RX_FRAME_COUNT)
    {
      unlock_flag(this->rx_rel_occupied);
      return false;
    }

    this->fill_ring[head & (RX_FRAME_COUNT - 1U)] = desc;
    this->fill_head.store(head + 1U, std::memory_order_release);
    unlock_flag(this->rx_rel_occupied);
    this->fill_head.notify_one();

    return true;
  }

  void Socket::wait_for_rx_release(io_packet_desc *desc) noexcept
  {
    for(;;)
    {
      const uint32_t observed_tail = this->fill_tail.load(std::memory_order_acquire);

      if(this->release_rx_packet(desc))
      {
        return;
      }

      if(this->fill_tail.load(std::memory_order_acquire) == observed_tail)
      {
        this->fill_tail.wait(observed_tail, std::memory_order_acquire);
      }
    }
  }

  // ================================================================
  // CONNECTION CLOSE
  // ================================================================

  void Socket::close_connection(int fd) noexcept
  {
    if(fd < 0)
    {
      return;
    }

    // All accepted client FDs are tracked by the io_uring thread. Do not close
    // an already-closed FD number a second time after the kernel reuses it.
    const auto it = this->active_fds.find(fd);

    if(it == this->active_fds.end())
    {
      return;
    }

    this->active_fds.erase(it);
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
  }

  // ================================================================
  // COMPLETION POLLING
  // ================================================================

  void Socket::rx_reciver(std::atomic<bool> &running) noexcept
  {
    if(!this->ring_initialized)
    {
      return;
    }

    __kernel_timespec timeout{ .tv_sec = 0, .tv_nsec = 1'000'000 };

    auto submit_send = [this](io_packet_desc *desc) noexcept -> bool
    {
      if(desc == nullptr || desc->client_fd < 0 || !this->active_fds.contains(desc->client_fd) || desc->len == 0 || desc->addr > static_cast<uint64_t>(TX_UMEM_LEN)
         || desc->len > static_cast<uint64_t>(TX_UMEM_LEN) - desc->addr)
      {
        return false;
      }

      io_uring_sqe *sqe = get_sqe_or_submit(this->iouring);

      if(sqe == nullptr)
      {
        return false;
      }

      io_uring_prep_send(sqe, desc->client_fd, this->tx_umem + static_cast<size_t>(desc->addr), static_cast<size_t>(desc->len), MSG_NOSIGNAL);
      io_uring_sqe_set_data(sqe, desc);
      return true;
    };

    while(running.load(std::memory_order_acquire))
    {
      // 1. Re-arm listeners whose previous multishot accept operation ended.
      for(std::size_t i = 0; i < this->accept_rearm_pending.size(); ++i)
      {
        if(this->accept_rearm_pending[i])
        {
          (void)this->arm_accept(i);
        }
      }

      // 2. Re-arm receives only after either an SQE shortage was resolved or
      //    an RX buffer has actually been provided back to the kernel.
      if(this->retry_pending_recv)
      {
        while(this->pending_recv_head < this->pending_recv_fds.size())
        {
          const int fd = this->pending_recv_fds[this->pending_recv_head];

          if(!this->active_fds.contains(fd))
          {
            ++this->pending_recv_head;
            continue;
          }

          if(!this->add_recv_request(fd))
          {
            break;
          }

          ++this->pending_recv_head;
        }

        if(this->pending_recv_head == this->pending_recv_fds.size())
        {
          this->pending_recv_fds.clear();
          this->pending_recv_head = 0;
          this->retry_pending_recv = false;
        }
        else if(this->pending_recv_head != 0)
        {
          this->pending_recv_fds.erase(this->pending_recv_fds.begin(), this->pending_recv_fds.begin() + static_cast<std::ptrdiff_t>(this->pending_recv_head));
          this->pending_recv_head = 0;
          this->retry_pending_recv = true;
        }
      }

      // 3. Submit TX descriptors published by HTTP workers.
      {
        uint32_t head = this->tx_send_head.load(std::memory_order_relaxed);
        const uint32_t tail = this->tx_send_tail.load(std::memory_order_acquire);

        while(head != tail)
        {
          io_packet_desc *desc = this->tx_pending_ring[head & (TX_FRAME_COUNT - 1U)];

          if(desc == nullptr)
          {
            std::println(stderr, "[error] null descriptor in TX pending queue");
            ++head;
            continue;
          }

          const auto frame_index_signed = desc - this->tx_ring.data();
          const bool belongs_to_tx_ring = frame_index_signed >= 0 && static_cast<std::size_t>(frame_index_signed) < this->tx_ring.size();

          if(!submit_send(desc))
          {
            break;
          }

          ++head;
        }

        this->tx_send_head.store(head, std::memory_order_release);
        this->tx_send_head.notify_all();
      }

      // 4. Re-provide RX frames returned by HTTP workers.
      {
        uint32_t tail = this->fill_tail.load(std::memory_order_relaxed);
        const uint32_t head = this->fill_head.load(std::memory_order_acquire);

        while(tail != head)
        {
          io_packet_desc *desc = this->fill_ring[tail & (RX_FRAME_COUNT - 1U)];

          io_uring_sqe *sqe = get_sqe_or_submit(this->iouring);

          const uint32_t bid = static_cast<uint32_t>(desc->addr / CHUNK_SIZE);

          io_uring_prep_provide_buffers(sqe, this->rx_umem + static_cast<size_t>(desc->addr), static_cast<int>(CHUNK_SIZE), 1, RX_GID, static_cast<int>(bid));
          io_uring_sqe_set_data64(sqe, PROVIDE_TAG | bid);

          ++tail;
        }

        this->fill_tail.store(tail, std::memory_order_release);
        this->fill_tail.notify_all();
      }

      // 5. Submit queued SQEs and wait briefly. The timeout lets the loop
      //    notice TX/RX queue publications even when no socket CQE arrives.
      io_uring_cqe *wait_cqe = nullptr;
      const int wait_result = io_uring_submit_and_wait_timeout(&this->iouring, &wait_cqe, 1, &timeout, nullptr);

      if(wait_result < 0 && wait_result != -ETIME && wait_result != -EINTR)
      {
        std::error_code ec(-wait_result, std::generic_category());
        std::println(stderr, "[error] io_uring_submit_and_wait_timeout: {} ({})", ec.message(), ec.value());
      }

      // 6. Process every CQE currently available.
      unsigned cqe_head = 0;
      unsigned cqe_count = 0;
      io_uring_cqe *cqe = nullptr;

      io_uring_for_each_cqe(&this->iouring, cqe_head, cqe)
      {
        ++cqe_count;

        const uint64_t user_data = io_uring_cqe_get_data64(cqe);
        const uint64_t tag = user_data & USER_TAG_MASK;
        const int result = cqe->res;

        if(tag == ACCEPT_TAG)
        {
          const auto listener_index = static_cast<std::size_t>(user_data & USER_DATA_MASK);

          if(result >= 0)
          {
            const int client_fd = result;

            try
            {
              this->active_fds.insert(client_fd);
            }
            catch(...)
            {
              ::shutdown(client_fd, SHUT_RDWR);
              ::close(client_fd);
              continue;
            }

            if(!this->add_recv_request(client_fd))
            {
              this->queue_pending_recv(client_fd, true);
            }
          }
          else if(result != -ECANCELED && running.load(std::memory_order_relaxed))
          {
            std::println(stderr, "[error] accept on listener {} failed: {}", listener_index, -result);
          }

          if((cqe->flags & IORING_CQE_F_MORE) == 0 && running.load(std::memory_order_relaxed))
          {
            this->accept_rearm_pending[listener_index] = true;
          }

          continue;
        }

        if(tag == PROVIDE_TAG)
        {
          const auto bid = static_cast<uint32_t>(user_data & 0xFFFFU);

          if(result < 0)
          {
            std::println(stderr, "[error] re-providing RX buffer {} failed: {}", bid, -result);

            if(bid < RX_FRAME_COUNT)
            {
              this->wait_for_rx_release(&this->rx_ring[bid]);
            }
          }
          else
          {
            // A buffer is back in the kernel's pool. Any receives stopped by
            // ENOBUFS can now be armed again.
            this->retry_pending_recv = true;
          }

          continue;
        }

        if(tag == RECV_TAG)
        {
          const int client_fd = static_cast<int>(user_data & 0xFFFF'FFFFULL);

          if(!this->active_fds.contains(client_fd))
          {
            continue;
          }

          if(result == -ENOBUFS)
          {
            // This multishot receive is now terminated. Re-arm after a buffer
            // has been returned to RX_GID.
            this->queue_pending_recv(client_fd, false);
            continue;
          }

          if(result < 0)
          {
            if(result != -ECANCELED)
            {
              std::println(stderr, "[error] recv fd {} failed: {}", client_fd, -result);
            }

            this->close_connection(client_fd);
            continue;
          }

          if(result == 0)
          {
            this->close_connection(client_fd);
            continue;
          }

          if((cqe->flags & IORING_CQE_F_BUFFER) == 0)
          {
            std::println(stderr, "[error] recv fd {} completed without a selected RX buffer", client_fd);
            this->close_connection(client_fd);
            continue;
          }

          const uint32_t bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;

          io_packet_desc *desc = &this->rx_ring[bid];
          desc->addr = static_cast<uint64_t>(bid) * static_cast<uint64_t>(CHUNK_SIZE);
          desc->client_fd = client_fd;
          desc->len = static_cast<uint32_t>(result);
          desc->flags = desc_event::PROCESSING;

          if(!HttpServer::http->enqueue(desc))
          {
            std::println(stderr, "[error] failed to enqueue RX descriptor (fd={})", client_fd);
            this->wait_for_rx_release(desc);
          }

          HttpServer::http->wakeup_rx();

          // A multishot receive with no F_MORE has ended. It must be re-armed,
          // but only after this CQE's buffer is returned to the provided pool.
          if((cqe->flags & IORING_CQE_F_MORE) == 0)
          {
            this->queue_pending_recv(client_fd, false);
          }

          continue;
        }

        // All remaining CQEs belong to TX SEND requests. Their user_data is
        // the exact descriptor pointer submitted with io_uring_prep_send().
        if(tag == 0)
        {
          auto *desc = reinterpret_cast<io_packet_desc *>(static_cast<uintptr_t>(user_data));

          if(result < 0)
          {
            if(result == -EAGAIN || result == -EINTR)
            {
              if(!submit_send(desc) && !this->release_tx_packet(desc))
              {
                std::println(stderr, "[error] could not requeue a transiently failed TX descriptor");
                this->close_connection(desc->client_fd);
                this->recycle_tx_packet(desc);
              }
            }
            else
            {
              std::println(stderr, "[error] send fd {} failed: {}", desc->client_fd, -result);
              this->close_connection(desc->client_fd);
              this->recycle_tx_packet(desc);
            }

            continue;
          }

          if(result == 0)
          {
            this->close_connection(desc->client_fd);
            this->recycle_tx_packet(desc);
            continue;
          }

          // Entire response was sent. The descriptor returns to the free pool.
          this->recycle_tx_packet(desc);
          continue;
        }

        std::println(stderr, "[error] unknown io_uring user_data tag: {:#x}", user_data);
      }

      if(cqe_count != 0)
      {
        io_uring_cq_advance(&this->iouring, cqe_count);
      }

      const int submit_result = io_uring_submit(&this->iouring);

      if(submit_result < 0 && submit_result != -EINTR)
      {
        std::error_code ec(-submit_result, std::generic_category());
        std::println(stderr, "[error] io_uring_submit: {} ({})", ec.message(), ec.value());
      }
    }
  }

  // ================================================================
  // CLEANUP
  // ================================================================

  void Socket::cleanup() noexcept
  {
    // The owner must stop and join rx_reciver() before destroying Socket.
    // Close accepted sockets first so outstanding multishot operations stop.
    for(const int fd : this->active_fds)
    {
      if(fd >= 0)
      {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
      }
    }

    this->active_fds.clear();

    for(int &fd : this->listen_fds)
    {
      if(fd >= 0)
      {
        ::close(fd);
        fd = -1;
      }
    }

    if(this->ring_initialized)
    {
      io_uring_queue_exit(&this->iouring);
      this->ring_initialized = false;
    }

    if(this->rx_umem != nullptr)
    {
      if(::munmap(this->rx_umem, static_cast<size_t>(RX_UMEM_LEN)) < 0)
      {
        std::println(stderr, "[error] munmap RX UMEM failed: {}", std::strerror(errno));
      }

      this->rx_umem = nullptr;
    }

    if(this->tx_umem != nullptr)
    {
      if(::munmap(this->tx_umem, static_cast<size_t>(TX_UMEM_LEN)) < 0)
      {
        std::println(stderr, "[error] munmap TX UMEM failed: {}", std::strerror(errno));
      }

      this->tx_umem = nullptr;
    }
  }

} // namespace HttpServer
