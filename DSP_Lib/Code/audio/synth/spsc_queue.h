#pragma once

#include <atomic>
#include <stdint.h>

namespace dsp {
namespace audio {
namespace synth {

// Single-producer single-consumer ring of T (lock-free; one thread pushes, one pops).
template <typename T, uint32_t N>
class SpscQueue {
 public:
  bool Push(const T& item) {
    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t next = (head + 1) % N;
    if (next == tail_.load(std::memory_order_acquire)) return false;
    buffer_[head] = item;
    head_.store(next, std::memory_order_release);
    return true;
  }
  bool Pop(T& item) {
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) return false;
    item = buffer_[tail];
    tail_.store((tail + 1) % N, std::memory_order_release);
    return true;
  }

 private:
  T buffer_[N];
  std::atomic<uint32_t> head_{0}, tail_{0};
};

}
}
}
