#pragma once

#include <atomic>
#include <cstddef>
#include <vector>

namespace HttpServer::Queue::SPSC
{
  template <size_t SIZE, typename T>
  class Queue
  {
  private:
    static constexpr uint32_t MOD = SIZE - 1;
    std::vector<T> ring;
    alignas(std::hardware_destructive_interference_size) std::atomic<uint32_t> head, tail, speek;

  public:
    Queue() : ring(std::vector<T>(SIZE, T{})), head(0), tail(0) {};
    ~Queue() = default;

    Queue(const Queue &) = delete;
    Queue(Queue &&) = delete;
    Queue &operator=(const Queue &) = delete;
    Queue &operator=(Queue &&) = delete;

  public:
    [[nodiscard]] bool dequeue(T &out)
    {
      const uint32_t current_head = head.load(std::memory_order_acquire);
      const uint32_t current_tail = tail.load(std::memory_order_acquire);

      if(current_head == current_tail)
      {
        return false; // nothing to dequeue
      }

      out = ring[current_tail & MOD];

      tail.store(current_tail + 1, std::memory_order_release);
      return true;
    }

    [[nodiscard]] bool enqueue(T val)
    {
      const uint32_t current_head = head.load(std::memory_order_acquire);
      const uint32_t current_tail = tail.load(std::memory_order_acquire);

      if(current_head - current_tail >= SIZE)
      {
        return false; // full
      }

      ring[current_head & MOD] = val;

      head.store(current_head + 1, std::memory_order_release);
      return true;
    }

    [[nodiscard]] bool peek(T &out)
    {
      const uint32_t current_head = head.load(std::memory_order_acquire);
      const uint32_t current_peek = speek.load(std::memory_order_acquire);

      if(current_head == current_peek)
      {
        return false; // nothing to dequeue
      }

      out = ring[current_peek & MOD];

      speek.store(current_peek + 1, std::memory_order_release);
      return true;
    }

    void sync()
    {
      const uint32_t current_speek = speek.load(std::memory_order_acquire);
      tail.store(current_speek, std::memory_order_seq_cst);
    }

    void reset()
    {
      const uint32_t current_tail = tail.load(std::memory_order_acquire);
      speek.store(current_tail, std::memory_order_seq_cst);
    }

    [[nodiscard]] size_t size() const
    {
      const uint32_t current_head = head.load(std::memory_order_acquire);
      const uint32_t current_tail = tail.load(std::memory_order_acquire);
      return current_head - current_tail;
    }
  };

} // namespace HttpServer::Queue::SPSC
