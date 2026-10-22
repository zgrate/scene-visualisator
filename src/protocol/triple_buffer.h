#pragma once
// Wait-free single-producer single-consumer handoff: the reader always gets the latest published value.
#include <atomic>
#include <cstdint>

namespace sv::proto {

template <class T> class TripleBuffer {
  public:
    // Producer: fill back(), then publish().
    T &back() { return buf_[back_]; }
    void publish() { back_ = middle_.exchange(std::uint8_t(back_ | dirty), std::memory_order_acq_rel) & index_mask; }

    // Consumer: update() swaps in the newest value if one was published since the last call.
    bool update() {
        if (!(middle_.load(std::memory_order_acquire) & dirty))
            return false;
        front_ = middle_.exchange(front_, std::memory_order_acq_rel) & index_mask;
        return true;
    }
    const T &front() const { return buf_[front_]; }

  private:
    static constexpr std::uint8_t dirty = 4, index_mask = 3;
    T buf_[3]{};
    std::uint8_t back_ = 0, front_ = 2;
    std::atomic<std::uint8_t> middle_{1};
};

} // namespace sv::proto
