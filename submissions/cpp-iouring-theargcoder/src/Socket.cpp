#include "include/Socket.hpp"
#include "include/Defines.hpp"
#include "include/HttpServer.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <thread>

namespace
{
  // Low two bits tag the completion kind.
  inline constexpr uint64_t USER_DATA_TAG_MASK = 0x3U;

  inline constexpr uint64_t USER_DATA_ACCEPT = 0x0U;
  inline constexpr uint64_t USER_DATA_RECV = 0x1U;
  inline constexpr uint64_t USER_DATA_SEND = 0x2U;

  // TX descriptor flags.
  inline constexpr uint32_t TX_COMPLETE_FLAG = 1U << 31U;
  inline constexpr uint32_t TX_CLOSE_ON_DONE_FLAG = 1U << 30U;

  inline constexpr uint32_t LISTENER_COUNT = 2U;

  [[nodiscard]] uint64_t make_accept_user_data(uint32_t listener_index) noexcept
  {
    return static_cast<uint64_t>(listener_index) << 2U;
  }

  [[nodiscard]] uint32_t listener_index_from_user_data(uint64_t user_data) noexcept
  {
    return static_cast<uint32_t>(user_data >> 2U);
  }

  [[nodiscard]] uint64_t make_recv_user_data(int fd) noexcept
  {
    return (static_cast<uint64_t>(static_cast<uint32_t>(fd)) << 2U) | USER_DATA_RECV;
  }

  [[nodiscard]] int fd_from_user_data(uint64_t user_data) noexcept
  {
    return static_cast<int>(static_cast<uint32_t>(user_data >> 2U));
  }

  [[nodiscard]] uint64_t make_send_user_data(uint32_t index) noexcept
  {
    return (static_cast<uint64_t>(index) << 2U) | USER_DATA_SEND;
  }

  [[nodiscard]] uint32_t tx_index_from_user_data(uint64_t user_data) noexcept
  {
    return static_cast<uint32_t>(user_data >> 2U);
  }

  [[nodiscard]] bool submit_pending(io_uring &ring) noexcept
  {
    int result;

    do
    {
      result = io_uring_submit(&ring);
    } while(result == -EINTR);

    if(result < 0)
    {
      std::println(stderr, "io_uring_submit failed: {}", std::strerror(-result));

      return false;
    }

    return result > 0;
  }

  // Called while sq_occupied is held.
  [[nodiscard]] io_uring_sqe *get_sqe(io_uring &ring) noexcept
  {
    io_uring_sqe *sqe = io_uring_get_sqe(&ring);

    if(!sqe)
    {
      if(!submit_pending(ring))
      {
        return nullptr;
      }

      sqe = io_uring_get_sqe(&ring);
    }

    return sqe;
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

  class SubmissionGuard
  {
  private:
    std::atomic_flag &flag;

  public:
    explicit SubmissionGuard(std::atomic_flag &lock) noexcept : flag(lock)
    {
      while(this->flag.test_and_set(std::memory_order_acquire))
      {
        std::this_thread::yield();
      }
    }

    ~SubmissionGuard() noexcept
    {
      this->flag.clear(std::memory_order_release);
    }

    SubmissionGuard(const SubmissionGuard &) = delete;
    SubmissionGuard &operator=(const SubmissionGuard &) = delete;
  };

  class AtomicBoolGuard
  {
  private:
    std::atomic<bool> &flag;

  public:
    explicit AtomicBoolGuard(std::atomic<bool> &lock) noexcept : flag(lock)
    {
      bool expected = false;

      while(!this->flag.compare_exchange_weak(expected, true, std::memory_order_acquire, std::memory_order_relaxed))
      {
        expected = false;
      }
    }

    ~AtomicBoolGuard() noexcept
    {
      this->flag.store(false, std::memory_order_release);
    }

    AtomicBoolGuard(const AtomicBoolGuard &) = delete;
    AtomicBoolGuard &operator=(const AtomicBoolGuard &) = delete;
  };

  class AtomicFlagGuard
  {
  private:
    std::atomic_flag &flag;

  public:
    explicit AtomicFlagGuard(std::atomic_flag &lock) noexcept : flag(lock)
    {
      while(this->flag.test_and_set(std::memory_order_acquire))
      {
        std::this_thread::yield();
      }
    }

    ~AtomicFlagGuard() noexcept
    {
      this->flag.clear(std::memory_order_release);
    }

    AtomicFlagGuard(const AtomicFlagGuard &) = delete;
    AtomicFlagGuard &operator=(const AtomicFlagGuard &) = delete;
  };

} // namespace

namespace HttpServer
{
  Socket::Socket()
  {
    try
    {
      // ================================================================
      // UMEM
      // ================================================================

      void *umem_addr = mmap(nullptr, UMEM_LEN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);

      if(umem_addr == MAP_FAILED)
      {
        perror_and_throw("Socket() - mmap UMEM failed");
      }

      this->umem = static_cast<char *>(umem_addr);

      // ================================================================
      // RX/TX DESCRIPTOR RINGS
      // ================================================================

      this->rx_ring = static_cast<io_packet_desc *>(std::calloc(RING_BUFFER_SIZE, sizeof(io_packet_desc)));

      this->tx_ring = static_cast<io_packet_desc *>(std::calloc(RING_BUFFER_SIZE, sizeof(io_packet_desc)));

      if(!this->rx_ring || !this->tx_ring)
      {
        throw std::runtime_error{ "Socket() - allocating packet descriptor rings failed" };
      }

      this->pending_recv_fds.fill(-1);

      // ================================================================
      // IO_URING
      // ================================================================

      io_uring_params params{};

      params.flags = IORING_SETUP_CQSIZE;
      params.cq_entries = RING_BUFFER_SIZE * 2U;

      const int init_result = io_uring_queue_init_params(RING_BUFFER_SIZE, &this->ring, &params);

      if(init_result < 0)
      {
        throw_io_uring_error("Socket() - io_uring_queue_init_params failed", init_result);
      }

      this->ring_initialized = true;

      // ================================================================
      // PROVIDED BUFFER RING
      // ================================================================

      int buf_ring_error = 0;

      this->buf_ring = io_uring_setup_buf_ring(&this->ring, RING_BUFFER_SIZE, BUF_GID, 0, &buf_ring_error);

      if(!this->buf_ring)
      {
        throw_io_uring_error("Socket() - io_uring_setup_buf_ring failed", buf_ring_error);
      }

      const int buf_mask = io_uring_buf_ring_mask(RING_BUFFER_SIZE);

      // Populate all RX frames.
      for(uint32_t i = 0; i < RX_FRAME_COUNT; ++i)
      {
        const uint64_t offset = static_cast<uint64_t>(i) * CHUNK_SIZE;

        io_uring_buf_ring_add(this->buf_ring, this->umem + offset, CHUNK_SIZE, static_cast<uint16_t>(i), buf_mask, static_cast<int>(i));
      }

      // Publish all initial buffers at once.
      io_uring_buf_ring_advance(this->buf_ring, RX_FRAME_COUNT);

      this->rx_buffers_available.store(RX_FRAME_COUNT, std::memory_order_relaxed);

      // ================================================================
      // LISTENERS
      // ================================================================

      this->listen_fds[0] = this->create_listener(static_cast<uint16_t>(std::atoi(PORT)));
      this->listen_fds[1] = this->create_listener(static_cast<uint16_t>(std::atoi(EXTRA_PORT)));

      // One multishot ACCEPT request per listener.
      if(!this->queue_accept(0))
      {
        throw std::runtime_error{ "Socket() - failed to queue initial accept for port 80" };
      }

      if(!this->queue_accept(1))
      {
        throw std::runtime_error{ "Socket() - failed to queue initial accept for port 81" };
      }

      std::println("Listening on 0.0.0.0:80 and 0.0.0.0:81");
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
  // LISTENER CREATION
  // ================================================================

  [[nodiscard]] int Socket::create_listener(uint16_t port)
  {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

    if(fd < 0)
    {
      perror_and_throw("Socket() - TCP socket creation failed");
    }

    int opt = 1;

    if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
      const int error = errno;

      close(fd);
      errno = error;

      perror_and_throw("Socket() - setsockopt(SO_REUSEADDR) failed");
    }

    sockaddr_in address{};

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(HOST);
    address.sin_port = htons(port);

    if(bind(fd, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) < 0)
    {
      const int error = errno;

      close(fd);
      errno = error;

      perror_and_throw("Socket() - TCP bind failed");
    }

    if(listen(fd, 65535) < 0)
    {
      const int error = errno;

      close(fd);
      errno = error;

      perror_and_throw("Socket() - TCP listen failed");
    }

    return fd;
  }

  // ================================================================
  // ACCEPT
  // ================================================================

  bool Socket::queue_accept(uint32_t listener_index) noexcept
  {
    if(listener_index >= LISTENER_COUNT)
    {
      return false;
    }

    const int listener_fd = this->listen_fds[listener_index];

    if(listener_fd < 0 || !this->ring_initialized)
    {
      return false;
    }

    SubmissionGuard guard{ this->sq_occupied };

    io_uring_sqe *sqe = get_sqe(this->ring);

    if(!sqe)
    {
      std::println(stderr, "io_uring SQ has no free entry for ACCEPT listener {}", listener_index);

      return false;
    }

    if(this->multishot_accept_supported)
    {
      io_uring_prep_multishot_accept(sqe, listener_fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    }
    else
    {
      // Compatibility fallback for kernels without multishot ACCEPT.
      io_uring_prep_accept(sqe, listener_fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    }

    io_uring_sqe_set_data64(sqe, make_accept_user_data(listener_index));

    return submit_pending(this->ring);
  }

  // ================================================================
  // MULTISHOT RECEIVE
  // ================================================================

  bool Socket::arm_multishot_recv(int fd) noexcept
  {
    if(fd < 0 || !this->ring_initialized)
    {
      return false;
    }

    SubmissionGuard guard{ this->sq_occupied };

    io_uring_sqe *sqe = get_sqe(this->ring);

    if(!sqe)
    {
      std::println(stderr, "io_uring SQ has no free entry for RECV fd={}", fd);

      return false;
    }

    if(this->multishot_recv_supported.load(std::memory_order_acquire))
    {
      io_uring_prep_recv_multishot(sqe, fd, nullptr, 0, 0);
    }
    else
    {
      // Single-shot fallback.
      io_uring_prep_recv(sqe, fd, nullptr, 0, 0);
    }

    sqe->flags |= IOSQE_BUFFER_SELECT;
    sqe->buf_group = BUF_GID;

    // Accepted connections normally start with no pending data.
    // Avoid an unnecessary initial receive attempt.
    sqe->ioprio |= IORING_RECVSEND_POLL_FIRST;

    io_uring_sqe_set_data64(sqe, make_recv_user_data(fd));

    return submit_pending(this->ring);
  }

  // ================================================================
  // TX ALLOCATION
  // ================================================================

  io_packet_desc *Socket::get_tx_packet_ptr() noexcept
  {
    if(!this->umem || !this->tx_ring)
    {
      return nullptr;
    }

    bool expected = false;

    if(!this->tx_occupied.compare_exchange_strong(expected, true, std::memory_order_acquire, std::memory_order_relaxed))
    {
      return nullptr;
    }

    const uint32_t head = this->tx_producer.load(std::memory_order_relaxed);
    const uint32_t tail = this->tx_consumer.load(std::memory_order_relaxed);
    const uint32_t outstanding = head - tail;

    if(outstanding >= RING_BUFFER_SIZE || outstanding >= TX_FRAME_COUNT)
    {
      this->tx_occupied.store(false, std::memory_order_release);

      return nullptr;
    }

    const uint32_t ring_index = head & (RING_BUFFER_SIZE - 1U);
    const uint64_t frame_index = static_cast<uint64_t>(head % TX_FRAME_COUNT);
    const uint64_t frame_offset = frame_index * CHUNK_SIZE;
    const uint64_t addr = TX_UMEM_BASE + frame_offset;

    if(addr < TX_UMEM_BASE || addr > UMEM_LEN || CHUNK_SIZE > UMEM_LEN - addr)
    {
      std::println(stderr, "TX UMEM allocation out of bounds: head={} frame={} addr={} chunk={} UMEM={}", head, frame_index, addr, CHUNK_SIZE, UMEM_LEN);

      this->tx_occupied.store(false, std::memory_order_release);

      return nullptr;
    }

    io_packet_desc &desc = this->tx_ring[ring_index];

    desc.addr = addr;
    desc.len = 0;
    desc.flags = 0;
    desc.client_fd = -1;

    this->tx_producer.store(head + 1U, std::memory_order_release);

    this->tx_occupied.store(false, std::memory_order_release);

    return &desc;
  }

  io_packet_desc *Socket::wait_for_tx_packet() noexcept
  {
    if(!this->umem || !this->tx_ring)
    {
      return nullptr;
    }

    for(;;)
    {
      if(auto *desc = this->get_tx_packet_ptr())
      {
        return desc;
      }

      const uint32_t observed = this->tx_consumer.load(std::memory_order_acquire);

      const uint32_t head = this->tx_producer.load(std::memory_order_acquire);

      const uint32_t outstanding = head - observed;

      if(outstanding < RING_BUFFER_SIZE && outstanding < TX_FRAME_COUNT)
      {
        // Another producer briefly owns the allocator.
        std::this_thread::yield();
        continue;
      }

      // Wait until CQE processing returns a TX descriptor.
      this->tx_consumer.wait(observed, std::memory_order_acquire);
    }
  }

  // ================================================================
  // TX SUBMISSION
  // ================================================================

  bool Socket::queue_send(io_packet_desc &desc) noexcept
  {
    constexpr uint64_t TX_BASE = static_cast<uint64_t>(TX_UMEM_BASE);
    constexpr uint64_t TX_SIZE = static_cast<uint64_t>(TX_FRAME_COUNT) * CHUNK_SIZE;
    constexpr uint64_t TX_END = TX_BASE + TX_SIZE;

    const bool invalid_range = TX_END > UMEM_LEN || desc.addr < TX_BASE || desc.addr >= TX_END || desc.addr > UMEM_LEN || desc.len > UMEM_LEN - desc.addr
                               || desc.len > TX_END - desc.addr || desc.len > CHUNK_SIZE - ((desc.addr - TX_BASE) % CHUNK_SIZE);

    if(!this->ring_initialized || desc.client_fd < 0 || desc.len == 0 || invalid_range)
    {
      std::println(stderr, "Invalid TX descriptor: fd={} addr={} len={}", desc.client_fd, desc.addr, desc.len);

      return false;
    }

    SubmissionGuard guard{ this->sq_occupied };

    io_uring_sqe *sqe = get_sqe(this->ring);

    if(!sqe)
    {
      return false;
    }

    io_uring_prep_send(sqe, desc.client_fd, this->umem + desc.addr, desc.len, MSG_NOSIGNAL);

    io_uring_sqe_set_data64(sqe, make_send_user_data(static_cast<uint32_t>(&desc - this->tx_ring)));

    return submit_pending(this->ring);
  }

  bool Socket::release_tx_packet(io_packet_desc &desc) noexcept
  {
    if(this->queue_send(desc))
    {
      return true;
    }

    // A failed submission will not produce a SEND CQE.
    const int fd = desc.client_fd;

    desc.flags |= TX_COMPLETE_FLAG;

    this->advance_tx_consumer();

    if(fd >= 0)
    {
      shutdown(fd, SHUT_RDWR);
      this->close_after_pending_sends(fd);
    }

    return false;
  }

  // ================================================================
  // TX RECLAMATION
  // ================================================================

  void Socket::advance_tx_consumer() noexcept
  {
    AtomicBoolGuard guard{ this->tx_occupied };

    uint32_t tail = this->tx_consumer.load(std::memory_order_relaxed);

    const uint32_t head = this->tx_producer.load(std::memory_order_acquire);

    while(tail != head)
    {
      const uint32_t index = tail & (RING_BUFFER_SIZE - 1U);

      io_packet_desc &desc = this->tx_ring[index];

      // Out-of-order SEND completions cannot be reclaimed past
      // the first incomplete descriptor.
      if((desc.flags & TX_COMPLETE_FLAG) == 0)
      {
        break;
      }

      desc.len = 0;
      desc.flags = 0;
      desc.client_fd = -1;

      ++tail;
    }

    this->tx_consumer.store(tail, std::memory_order_release);

    this->tx_consumer.notify_all();
  }

  // ================================================================
  // PENDING RECEIVE REARM
  // ================================================================

  void Socket::defer_recv_rearm(int fd) noexcept
  {
    if(fd < 0)
    {
      return;
    }

    bool overflow = false;

    {
      AtomicFlagGuard guard{ this->pending_recv_occupied };

      if(this->pending_recv_count >= PENDING_RECV_QUEUE_SIZE)
      {
        overflow = true;
      }
      else
      {
        const uint32_t index = this->pending_recv_tail;

        this->pending_recv_fds[index] = fd;

        this->pending_recv_tail = (this->pending_recv_tail + 1U) & (PENDING_RECV_QUEUE_SIZE - 1U);

        ++this->pending_recv_count;
      }
    }

    if(overflow)
    {
      std::println(stderr, "Pending RECV rearm queue full for fd={}", fd);

      this->close_after_pending_sends(fd);
    }
  }

  void Socket::rearm_pending_receives() noexcept
  {
    while(this->rx_buffers_available.load(std::memory_order_acquire) > 0)
    {
      int fd = -1;

      {
        AtomicFlagGuard guard{ this->pending_recv_occupied };

        if(this->pending_recv_count == 0)
        {
          return;
        }

        fd = this->pending_recv_fds[this->pending_recv_head];

        this->pending_recv_fds[this->pending_recv_head] = -1;

        this->pending_recv_head = (this->pending_recv_head + 1U) & (PENDING_RECV_QUEUE_SIZE - 1U);

        --this->pending_recv_count;
      }

      if(fd < 0)
      {
        continue;
      }

      if(!this->arm_multishot_recv(fd))
      {
        // SQ temporarily unavailable. Put the connection back.
        this->defer_recv_rearm(fd);
        return;
      }
    }
  }

  void Socket::remove_pending_recv_rearm(int fd) noexcept
  {
    if(fd < 0)
    {
      return;
    }

    AtomicFlagGuard guard{ this->pending_recv_occupied };

    uint32_t kept = 0;

    const uint32_t original_count = this->pending_recv_count;

    for(uint32_t i = 0; i < original_count; ++i)
    {
      const uint32_t index = (this->pending_recv_head + i) & (PENDING_RECV_QUEUE_SIZE - 1U);

      const int pending_fd = this->pending_recv_fds[index];

      if(pending_fd != fd)
      {
        const uint32_t destination = (this->pending_recv_head + kept) & (PENDING_RECV_QUEUE_SIZE - 1U);

        this->pending_recv_fds[destination] = pending_fd;

        ++kept;
      }
    }

    this->pending_recv_count = kept;

    this->pending_recv_tail = (this->pending_recv_head + kept) & (PENDING_RECV_QUEUE_SIZE - 1U);
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

    this->remove_pending_recv_rearm(fd);

    shutdown(fd, SHUT_RDWR);
    close(fd);

    if(this->conn_fd == fd)
    {
      this->conn_fd = -1;
    }
  }

  void Socket::close_after_pending_sends(int fd) noexcept
  {
    if(fd < 0)
    {
      return;
    }

    bool pending = false;

    // First account for requests still owned by RX/DB workers.
    {
      AtomicBoolGuard guard{ this->rx_occupied };

      const uint32_t tail = this->rx_consumer.load(std::memory_order_relaxed);

      const uint32_t head = this->rx_producer.load(std::memory_order_acquire);

      for(uint32_t sequence = tail; sequence != head; ++sequence)
      {
        const uint32_t index = sequence & (RING_BUFFER_SIZE - 1U);

        if(this->rx_slot_released[index].load(std::memory_order_acquire))
        {
          continue;
        }

        io_packet_desc &desc = this->rx_ring[index];

        if(desc.client_fd == fd)
        {
          this->rx_close_after[index].store(true, std::memory_order_release);

          pending = true;
        }
      }
    }

    // Then account for SEND operations already submitted.
    {
      AtomicBoolGuard guard{ this->tx_occupied };

      const uint32_t tail = this->tx_consumer.load(std::memory_order_relaxed);
      const uint32_t head = this->tx_producer.load(std::memory_order_acquire);

      for(uint32_t sequence = tail; sequence != head; ++sequence)
      {
        const uint32_t index = sequence & (RING_BUFFER_SIZE - 1U);

        io_packet_desc &desc = this->tx_ring[index];

        if(desc.client_fd == fd && (desc.flags & TX_COMPLETE_FLAG) == 0)
        {
          desc.flags |= TX_CLOSE_ON_DONE_FLAG;
          pending = true;
        }
      }
    }

    if(!pending)
    {
      this->close_connection(fd);
    }
  }

  // ================================================================
  // RX RECLAMATION
  // ================================================================

  void Socket::release_rx_packet(const io_packet_desc *desc) noexcept
  {
    if(!desc || !this->rx_ring || !this->buf_ring || !this->umem)
    {
      return;
    }

    const auto slot_offset = desc - this->rx_ring;

    if(slot_offset < 0 || static_cast<size_t>(slot_offset) >= RING_BUFFER_SIZE)
    {
      std::println(stderr, "Invalid RX descriptor pointer");
      return;
    }

    const uint32_t slot = static_cast<uint32_t>(slot_offset);

    const int client_fd = desc->client_fd;

    bool close_after = false;
    bool recycled = false;
    bool invalid_buffer = false;

    {
      AtomicBoolGuard guard{ this->rx_occupied };

      if(this->rx_slot_released[slot].load(std::memory_order_acquire))
      {
        std::println(stderr, "Duplicate RX release: slot={} fd={}", slot, client_fd);

        return;
      }

      close_after = this->rx_close_after[slot].exchange(false, std::memory_order_acq_rel);

      // Mark the descriptor released. Its buffer is not returned until FIFO reclamation.
      this->rx_slot_released[slot].store(true, std::memory_order_release);
      uint32_t tail = this->rx_consumer.load(std::memory_order_relaxed);
      const uint32_t head = this->rx_producer.load(std::memory_order_acquire);
      const int mask = io_uring_buf_ring_mask(RING_BUFFER_SIZE);

      while(tail != head)
      {
        const uint32_t index = tail & (RING_BUFFER_SIZE - 1U);

        if(!this->rx_slot_released[index].load(std::memory_order_acquire))
        {
          break;
        }

        io_packet_desc &released = this->rx_ring[index];

        const uint64_t addr = released.addr;

        const bool valid_buffer = addr % CHUNK_SIZE == 0 && addr <= UMEM_LEN && CHUNK_SIZE <= UMEM_LEN - addr && addr / CHUNK_SIZE < RX_FRAME_COUNT;

        if(!valid_buffer)
        {
          std::println(stderr, "Cannot recycle invalid RX buffer: addr={}", addr);

          invalid_buffer = true;
          break;
        }

        const uint16_t buffer_id = static_cast<uint16_t>(addr / CHUNK_SIZE);

        // Add one buffer at the current provided-buffer-ring tail.
        io_uring_buf_ring_add(this->buf_ring, this->umem + addr, CHUNK_SIZE, buffer_id, mask, 0);
        io_uring_buf_ring_advance(this->buf_ring, 1);
        this->rx_buffers_available.fetch_add(1, std::memory_order_relaxed);
        // Keep rx_slot_released true until this descriptor slot is reused by the RX producer.

        ++tail;
        recycled = true;
      }

      this->rx_consumer.store(tail, std::memory_order_release);
    }

    // Do connection work outside the RX critical section.
    if(close_after)
    {
      this->close_after_pending_sends(client_fd);
    }

    if(invalid_buffer)
    {
      this->close_after_pending_sends(client_fd);
      return;
    }

    if(recycled)
    {
      this->rearm_pending_receives();
    }
  }

  // ================================================================
  // TX/RX COMPLETION POLLING
  // ================================================================

  io_packet_desc *Socket::wait_for_rx_packet(std::atomic<bool> &signal) noexcept
  {
    while(signal.load(std::memory_order_seq_cst))
    {
      io_uring_cqe *cqe = nullptr;

      // Preserve the existing 1 ms RX-worker wakeup contract.
      __kernel_timespec timeout = { .tv_sec = 0, .tv_nsec = 1'000'000 };
      const int wait_result = io_uring_wait_cqe_timeout(&this->ring, &cqe, &timeout);

      if(wait_result < 0)
      {
        if(wait_result == -ETIME)
        {
          HttpServer::http->wakeup_rx();
          this->rearm_pending_receives();
          continue;
        }

        if(wait_result == -EINTR)
        {
          continue;
        }

        std::println(stderr, "io_uring_wait_cqe failed: {}", std::strerror(-wait_result));

        return nullptr;
      }

      const int result = cqe->res;
      const uint32_t cqe_flags = cqe->flags;

      const uint64_t user_data = io_uring_cqe_get_data64(cqe);

      const uint64_t tag = user_data & USER_DATA_TAG_MASK;

      io_packet_desc *packet_desc = nullptr;

      // ============================================================
      // ACCEPT COMPLETION
      // ============================================================

      if(tag == USER_DATA_ACCEPT)
      {
        const uint32_t listener_index = listener_index_from_user_data(user_data);

        const bool more = (cqe_flags & IORING_CQE_F_MORE) != 0;

        if(listener_index >= LISTENER_COUNT)
        {
          std::println(stderr, "ACCEPT CQE has invalid listener index: {}", listener_index);
        }
        else if(result >= 0)
        {
          const int accepted_fd = result;

          this->conn_fd = accepted_fd;

          if(!this->arm_multishot_recv(accepted_fd))
          {
            std::println(stderr, "Failed to arm RECV for fd={}", accepted_fd);

            this->close_connection(accepted_fd);
          }
        }
        else if(result == -EINVAL && this->multishot_accept_supported)
        {
          // Kernel does not support multishot ACCEPT. Switch to ordinary single-shot accept.
          this->multishot_accept_supported = false;

          std::println(stderr, "Multishot ACCEPT unsupported; using single-shot ACCEPT");
        }
        else if(result != -ECANCELED)
        {
          std::println(stderr, "io_uring ACCEPT failed on listener {}: {}", listener_index, std::strerror(-result));
        }

        // Multishot ACCEPT remains active while F_MORE is set. Single-shot ACCEPT must always be rearmed.
        if(!more && result != -ECANCELED)
        {
          if(!this->queue_accept(listener_index))
          {
            std::println(stderr, "Failed to re-arm ACCEPT listener {}", listener_index);
          }
        }
      }

      // ============================================================
      // RECV COMPLETION
      // ============================================================

      else if(tag == USER_DATA_RECV)
      {
        const int client_fd = fd_from_user_data(user_data);

        const bool more = (cqe_flags & IORING_CQE_F_MORE) != 0;

        if(result > 0)
        {
          if((cqe_flags & IORING_CQE_F_BUFFER) == 0)
          {
            std::println(stderr, "RECV CQE has no provided-buffer ID (fd={} flags=0x{:x})", client_fd, cqe_flags);

            this->close_after_pending_sends(client_fd);
          }
          else
          {
            const uint32_t buffer_id = cqe_flags >> IORING_CQE_BUFFER_SHIFT;

            if(buffer_id >= RX_FRAME_COUNT)
            {
              std::println(stderr, "RECV returned invalid buffer ID {} for fd={}", buffer_id, client_fd);

              this->close_after_pending_sends(client_fd);
            }
            else
            {
              const uint64_t offset = static_cast<uint64_t>(buffer_id) * CHUNK_SIZE;
              this->rx_buffers_available.fetch_sub(1, std::memory_order_relaxed);

              bool overflow = false;
              AtomicBoolGuard guard{ this->rx_occupied };

              const uint32_t head = this->rx_producer.load(std::memory_order_relaxed);
              const uint32_t tail = this->rx_consumer.load(std::memory_order_acquire);

              if(head - tail >= RING_BUFFER_SIZE)
              {
                overflow = true;

                // This should not happen when buffers are only returned after FIFO descriptor reclamation. Return the unassigned buffer rather than leak it.

                const int mask = io_uring_buf_ring_mask(RING_BUFFER_SIZE);

                io_uring_buf_ring_add(this->buf_ring, this->umem + offset, CHUNK_SIZE, static_cast<uint16_t>(buffer_id), mask, 0);
                io_uring_buf_ring_advance(this->buf_ring, 1);

                this->rx_buffers_available.fetch_add(1, std::memory_order_relaxed);
              }
              else
              {
                const uint32_t index = head & (RING_BUFFER_SIZE - 1U);

                this->rx_slot_released[index].store(false, std::memory_order_relaxed);

                this->rx_close_after[index].store(false, std::memory_order_relaxed);

                this->rx_ring[index].addr = offset;
                this->rx_ring[index].len = static_cast<uint32_t>(result);

                this->rx_ring[index].flags = cqe_flags;
                this->rx_ring[index].client_fd = client_fd;

                packet_desc = &this->rx_ring[index];

                this->rx_producer.store(head + 1U, std::memory_order_release);
              }

              if(overflow)
              {
                std::println(stderr, "RX descriptor ownership invariant violated: fd={}", client_fd);

                this->close_after_pending_sends(client_fd);
              }
            }
          }

          // A positive completion without MORE means this receive operation terminated. Re-arm it if the connection remains.
          if(!more)
          {
            if(!this->arm_multishot_recv(client_fd))
            {
              this->close_after_pending_sends(client_fd);
            }
          }
        }
        else if(result == 0)
        {
          // TCP EOF.
          this->close_after_pending_sends(client_fd);
        }
        else if(result == -ENOBUFS && !more)
        {
          // Multishot RECV stopped because no provided buffers were available. Rearm when buffers return.
          this->defer_recv_rearm(client_fd);
        }
        else if(result == -EINVAL && !more && this->multishot_recv_supported.exchange(false, std::memory_order_acq_rel))
        {
          // Kernel does not support multishot RECV. Re-arm using the single-shot fallback.
          std::println(stderr, "Multishot RECV unsupported; using single-shot RECV");

          if(!this->arm_multishot_recv(client_fd))
          {
            this->close_after_pending_sends(client_fd);
          }
        }
        else if(result == -ECANCELED)
        {
          // Expected when a connection is being closed.
        }
        else if(!more)
        {
          std::println(stderr, "io_uring RECV failed for fd={}: {}", client_fd, std::strerror(-result));

          this->close_after_pending_sends(client_fd);
        }
      }

      // ============================================================
      // SEND COMPLETION
      // ============================================================

      else if(tag == USER_DATA_SEND)
      {
        const uint32_t index = tx_index_from_user_data(user_data);

        if(index < RING_BUFFER_SIZE)
        {
          io_packet_desc &tx_desc = this->tx_ring[index];

          if(result > 0 && static_cast<uint32_t>(result) < tx_desc.len)
          {
            // Partial SEND. Continue with the remaining bytes.
            tx_desc.addr += static_cast<uint64_t>(result);
            tx_desc.len -= static_cast<uint32_t>(result);

            if(!this->release_tx_packet(tx_desc))
            {
              std::println(stderr, "Failed to resubmit partial SEND for fd={}", tx_desc.client_fd);
            }
          }
          else
          {
            const int completed_fd = tx_desc.client_fd;

            const bool close_after = (tx_desc.flags & TX_CLOSE_ON_DONE_FLAG) != 0;

            const bool send_failed = result <= 0 || static_cast<uint32_t>(result) > tx_desc.len;

            if(send_failed)
            {
              const int error = result < 0 ? -result : EPIPE;

              std::println(stderr, "io_uring SEND failed for fd={}: {}", completed_fd, std::strerror(error));

              if(completed_fd >= 0)
              {
                shutdown(completed_fd, SHUT_RDWR);
              }
            }

            tx_desc.flags |= TX_COMPLETE_FLAG;

            this->advance_tx_consumer();

            if(close_after || send_failed)
            {
              this->close_after_pending_sends(completed_fd);
            }
          }
        }
        else
        {
          std::println(stderr, "SEND CQE has invalid TX descriptor index: {}", index);
        }
      }

      else
      {
        std::println(stderr, "Unknown io_uring completion tag: {}", static_cast<unsigned long long>(tag));
      }

      // Every CQE is consumed exactly once.
      io_uring_cqe_seen(&this->ring, cqe);

      // Return only actual RX packets to the caller. ACCEPT and SEND completions are handled internally.
      if(packet_desc)
      {
        return packet_desc;
      }
    }

    return nullptr;
  }

  // ================================================================
  // CLEANUP
  // ================================================================

  void Socket::cleanup() noexcept
  {
    // Stop outstanding ACCEPT/RECV/SEND operations first.
    if(this->ring_initialized)
    {
      io_uring_queue_exit(&this->ring);
      this->ring_initialized = false;
    }

    // io_uring_setup_buf_ring() owns the buffer-ring mapping. After queue shutdown, the registration is gone; unmap its userspace allocation.
    if(this->buf_ring)
    {
      const size_t buf_ring_size = static_cast<size_t>(RING_BUFFER_SIZE) * sizeof(io_uring_buf);

      if(munmap(this->buf_ring, buf_ring_size) < 0)
      {
        std::println(stderr, "munmap provided buffer ring failed: {}", std::strerror(errno));
      }

      this->buf_ring = nullptr;
    }

    // Close listeners.
    for(int &fd : this->listen_fds)
    {
      if(fd >= 0)
      {
        close(fd);
        fd = -1;
      }
    }

    // Preserve the original last-accepted-FD cleanup behavior.
    if(this->conn_fd >= 0)
    {
      close(this->conn_fd);
      this->conn_fd = -1;
    }

    if(this->umem)
    {
      munmap(this->umem, UMEM_LEN);
      this->umem = nullptr;
    }

    std::free(this->rx_ring);
    this->rx_ring = nullptr;

    std::free(this->tx_ring);
    this->tx_ring = nullptr;

    this->rx_consumer.store(0);
    this->rx_producer.store(0);

    this->tx_consumer.store(0);
    this->tx_producer.store(0);

    this->rx_buffers_available.store(RX_FRAME_COUNT);

    this->pending_recv_head = 0;
    this->pending_recv_tail = 0;
    this->pending_recv_count = 0;
  }

} // namespace HttpServer
