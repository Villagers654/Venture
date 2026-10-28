#include "vtest.h"
#include <atomic>
#include <thread>
#include <vector>

// Job queues need exclusive-monitor coherence, outline helpers and ordering.
template<class T> static bool width() {
    std::atomic<T> value{T(1)};
    T expected = 1;
    if (!value.compare_exchange_strong(expected, T(7))) return false;
    expected = 2;
    if (value.compare_exchange_strong(expected, T(9)) || expected != T(7)) return false;
    return value.fetch_add(T(3)) == T(7) && value.exchange(T(2)) == T(10) && value.load() == T(2);
}
#if defined(__aarch64__)
// Calling through a noinline helper forces libatomic's compare_exchange_16
// fallback instead of allowing clang to inline every 128-bit operation.
extern "C" bool vtest_compare_16(volatile void*, void*, unsigned __int128, int, int)
    __asm__("__atomic_compare_exchange_16");
static __attribute__((noinline)) bool wide_cas(unsigned __int128* value, unsigned __int128* expected, unsigned __int128 desired) {
    return vtest_compare_16(value, expected, desired, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}
#endif
extern "C" int vtest_main(int, char**) {
    Vtest q("atomics");
    CHECK(width<uint8_t>() && width<uint16_t>() && width<uint32_t>() && width<uint64_t>());
    std::atomic<bool> boolean{false}; bool no = false;
    CHECK(boolean.compare_exchange_strong(no, true) && boolean.load());
    int first = 1, second = 2; std::atomic<int*> pointer{&first};
    CHECK(pointer.exchange(&second) == &first && *pointer.load() == 2);
#if defined(__aarch64__)
    alignas(16) unsigned __int128 wide = (static_cast<unsigned __int128>(5) << 64) | 1;
    unsigned __int128 expected = wide, desired = (static_cast<unsigned __int128>(6) << 64) | 2;
    CHECK(wide_cas(&wide, &expected, desired) && wide == desired);
    expected = 0; CHECK(!wide_cas(&wide, &expected, 7) && expected == desired);
    std::atomic<unsigned __int128> standard{desired};
    CHECK(standard.exchange(3) == desired && standard.load() == 3);
#else
    // Clang's ARM32 target has no __int128 type; 64-bit is its widest atomic.
    CHECK(sizeof(uint64_t) == 8);
#endif
    q.step("atomic: widths bool pointer wide CAS OK");
    std::atomic<uint64_t> counter{0}; std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    for (int i = 0; i < 8; ++i) workers.emplace_back([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (int n = 0; n < 10000; ++n) {
            uint64_t old = counter.load(std::memory_order_relaxed);
            while (!counter.compare_exchange_weak(old, old + 1, std::memory_order_relaxed)) {}
        }
    });
    start.store(true, std::memory_order_release);
    for (auto& t : workers) t.join(); CHECK(counter == 80000);
    q.step("atomic: eight-thread CAS contention OK");
    std::atomic<int> state{0}; int payload = 0; bool valid = true;
    std::thread consumer([&] {
        for (int i = 1; i <= 1000; ++i) {
            while (state.load(std::memory_order_acquire) != 1) std::this_thread::yield();
            if (payload != i * 7) valid = false;
            state.store(0, std::memory_order_release);
        }
    });
    for (int i = 1; i <= 1000; ++i) {
        while (state.load(std::memory_order_acquire) != 0) std::this_thread::yield();
        payload = i * 7; state.store(1, std::memory_order_release);
    }
    consumer.join(); CHECK(valid);
    q.step("atomic: acquire release message passing OK");
    return q.finish();
}
