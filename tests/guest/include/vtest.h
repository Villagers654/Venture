#pragma once
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <cstddef>
#include <initializer_list>

// One fixed checkpoint per group. Failure reasons identify checks, never include
// host-dependent errno text, addresses, thread IDs, clocks or scheduler order.
struct Vtest {
    const char* name;
    const char* failure = nullptr;
    explicit Vtest(const char* value) : name(value) {}
    bool check(bool ok, const char* reason) {
        if (!ok && !failure) failure = reason;
        return ok;
    }
    void step(const char* text) { if (!failure) std::puts(text); }
    int finish() {
        if (failure) {
            std::printf("VTEST RESULT %s FAIL %s\n", name, failure);
            std::fflush(stdout);
            return 1;
        }
        std::printf("VTEST RESULT %s PASS\n", name);
        std::fflush(stdout);
        return 0;
    }
};
#define CHECK(expr) q.check(bool(expr), #expr)
#define REQUIRE(expr) do { if (!CHECK(expr)) return q.finish(); } while (0)
inline timespec vtest_deadline(clockid_t clock = CLOCK_REALTIME, int seconds = 5) {
    timespec result{};
    clock_gettime(clock, &result);
    result.tv_sec += seconds;
    return result;
}
inline bool vtest_timespec_valid(const timespec& t) {
    return t.tv_sec >= 0 && t.tv_nsec >= 0 && t.tv_nsec < 1000000000;
}
