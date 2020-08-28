#pragma once

#include <cstdint>
#include <cassert>
#include <atomic>
#include <ctime>

inline void asm_volatile_pause() {
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    ::_mm_pause();
#elif defined(__i386__) || FOLLY_X64 || (__mips_isa_rev > 1)
      asm volatile("pause");
#elif FOLLY_AARCH64 || (defined(__arm__) && !(__ARM_ARCH < 7))
        asm volatile("yield");
#elif FOLLY_PPC64
          asm volatile("or 27,27,27");
#endif
}

/*
 * A helper object for the contended case. Starts off with eager
 * spinning, and falls back to sleeping for small quantums.
 */
class Sleeper {
  static const uint32_t kMaxActiveSpin = 4000;

  uint32_t spinCount;

 public:
  Sleeper() noexcept : spinCount(0) {}

  static void sleep() noexcept {
    /*
     * Always sleep 0.5ms, assuming this will make the kernel put
     * us down for whatever its minimum timer resolution is (in
     * linux this varies by kernel version from 1ms to 10ms).
     */
    struct timespec ts = {0, 500000};
    nanosleep(&ts, nullptr);
  }

  void wait() noexcept {
    if (spinCount < kMaxActiveSpin) {
      ++spinCount;
      asm_volatile_pause();
    } else {
      sleep();
    }
  }
};

/*
 * A really, *really* small spinlock for fine-grained locking of lots
 * of teeny-tiny data.
 *
 * Zero initializing these is guaranteed to be as good as calling
 * init(), since the free state is guaranteed to be all-bits zero.
 *
 * This class should be kept a POD, so we can used it in other packed
 * structs (gcc does not allow __attribute__((__packed__)) on structs that
 * contain non-POD data).  This means avoid adding a constructor, or
 * making some members private, etc.
 */
struct MicroSpinLock {
  enum { FREE = 0, LOCKED = 1 };
  // lock_ can't be std::atomic<> to preserve POD-ness.
  uint8_t lock_;

  // Initialize this MSL.  It is unnecessary to call this if you
  // zero-initialize the MicroSpinLock.
  void init() noexcept {
    payload()->store(FREE);
  }

  bool try_lock() noexcept {
    bool ret = cas(FREE, LOCKED);
    return ret;
  }

  void lock() noexcept {
    Sleeper sleeper;
    while (!cas(FREE, LOCKED)) {
      do {
        sleeper.wait();
      } while (payload()->load(std::memory_order_relaxed) == LOCKED);
    }
    assert(payload()->load() == LOCKED);
  }

  void unlock() noexcept {
    assert(payload()->load() == LOCKED);
    payload()->store(FREE, std::memory_order_release);
  }

 private:
  std::atomic<uint8_t>* payload() noexcept {
    return reinterpret_cast<std::atomic<uint8_t>*>(&this->lock_);
  }

  bool cas(uint8_t compare, uint8_t newVal) noexcept {
    return std::atomic_compare_exchange_strong_explicit(
        payload(),
        &compare,
        newVal,
        std::memory_order_acquire,
        std::memory_order_relaxed);
  }
};
static_assert(
    std::is_pod<MicroSpinLock>::value,
    "MicroSpinLock must be kept a POD type.");
