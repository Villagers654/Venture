#include "vtest.h"
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <linux/futex.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>

// HLE calls back into guest comparators; engine IO loops expect Linux descriptors.
static int compare(const void* a, const void* b) {
    int x = *static_cast<const int*>(a), y = *static_cast<const int*>(b);
    return (x > y) - (x < y);
}
extern "C" int vtest_main(int, char**) {
    Vtest q("misc");
    const char* old = getenv("VTEST_GUEST_ENV"); char* saved = old ? strdup(old) : nullptr;
    REQUIRE(!old || saved);
    CHECK(setenv("VTEST_GUEST_ENV", "quest", 1) == 0);
    const char* value = getenv("VTEST_GUEST_ENV"); CHECK(value && strcmp(value, "quest") == 0);
    CHECK(setenv("VTEST_GUEST_ENV", "port", 0) == 0);
    value = getenv("VTEST_GUEST_ENV"); CHECK(value && strcmp(value, "quest") == 0);
    CHECK(unsetenv("VTEST_GUEST_ENV") == 0 && getenv("VTEST_GUEST_ENV") == nullptr);
    if (saved) { CHECK(setenv("VTEST_GUEST_ENV", saved, 1) == 0); free(saved); }
    int items[] = {9, -2, 4, 0, 4, 1}; qsort(items, 6, sizeof(int), compare);
    int sorted[] = {-2, 0, 1, 4, 4, 9}; CHECK(memcmp(items, sorted, sizeof(items)) == 0);
    int key = 9; auto found = static_cast<int*>(bsearch(&key, items, 6, sizeof(int), compare));
    CHECK(found && *found == 9); key = 7; CHECK(bsearch(&key, items, 6, sizeof(int), compare) == nullptr);
    unsigned seed1 = 12345, seed2 = 12345; bool different = false; int previous = -1;
    for (int i = 0; i < 16; ++i) { int a = rand_r(&seed1), b = rand_r(&seed2); CHECK(a == b && a >= 0); different |= i > 0 && a != previous; previous = a; }
    CHECK(different);
    long sequence[8]; srandom(12345); for (auto& n : sequence) n = random();
    srandom(12345); for (long n : sequence) CHECK(n == random());
    different = false; for (int i = 1; i < 8; ++i) different |= sequence[i] != sequence[0]; CHECK(different);
    q.step("libc: environment guest callbacks deterministic random OK");
    std::atomic<bool> ready{false}, release{false}, errno_ok{false};
    std::thread errno_thread([&] {
        errno = ERANGE; ready.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
        errno_ok = errno == ERANGE;
    });
    errno = EDOM;
    while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    CHECK(errno == EDOM); release.store(true, std::memory_order_release); errno_thread.join(); CHECK(errno_ok.load());
    CHECK(sysconf(_SC_NPROCESSORS_ONLN) >= 1);
    long page = sysconf(_SC_PAGESIZE); CHECK(page >= 4096 && (page & (page - 1)) == 0);
    utsname system{}; CHECK(uname(&system) == 0 && strcmp(system.sysname, "Linux") == 0);
    CHECK(getpid() > 0 && syscall(SYS_gettid) == gettid());
    // Raw sched_getattr/sched_setattr (no bionic wrappers): an app thread is
    // SCHED_OTHER and may keep it, but cannot take a real-time class.
    struct { uint32_t size, policy; uint64_t flags; int32_t nice; uint32_t priority; uint64_t runtime, deadline, period; uint32_t util_min, util_max; } attr{};
    CHECK(syscall(SYS_sched_getattr, 0, &attr, sizeof attr, 0) == 0 && attr.size == sizeof attr && attr.policy == SCHED_OTHER);
    CHECK(syscall(SYS_sched_getattr, 0, &attr, 8, 0) == -1 && errno == EINVAL);
    CHECK(syscall(SYS_sched_setattr, 0, &attr, 0) == 0);
    attr.policy = SCHED_FIFO; attr.priority = 10;
    CHECK(syscall(SYS_sched_setattr, 0, &attr, 0) == -1 && errno == EPERM);
    char old_name[16] = {}, name[16] = {}; CHECK(prctl(PR_GET_NAME, old_name, 0, 0, 0) == 0);
    CHECK(prctl(PR_SET_NAME, "vtest-main", 0, 0, 0) == 0 && prctl(PR_GET_NAME, name, 0, 0, 0) == 0);
    CHECK(strcmp(name, "vtest-main") == 0); CHECK(prctl(PR_SET_NAME, old_name, 0, 0, 0) == 0);
    q.step("libc: errno TLS sysconf uname IDs prctl OK");
    alignas(4) uint32_t word = 0; std::atomic<bool> waiting{false}, finished{false};
    long wait_result = -1;
    std::thread waiter([&] {
        timespec timeout{5, 0}; waiting.store(true, std::memory_order_release);
        wait_result = syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, 0, &timeout, nullptr, 0);
        finished.store(true, std::memory_order_release);
    });
    while (!waiting.load(std::memory_order_acquire)) std::this_thread::yield();
    long woke = 0;
    while (!finished.load(std::memory_order_acquire)) {
        woke = syscall(SYS_futex, &word, FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
        if (woke != 0) break;
        sched_yield();
    }
    waiter.join(); CHECK(woke == 1 && wait_result == 0);
    q.step("libc: futex blocked wait wake OK");
    int pipes[2]; REQUIRE(pipe(pipes) == 0);
    CHECK(isatty(pipes[0]) == 0);
    pollfd poller{pipes[0], POLLIN, 0}; CHECK(poll(&poller, 1, 0) == 0);
    CHECK(write(pipes[1], "io", 2) == 2 && poll(&poller, 1, 1000) == 1 && (poller.revents & POLLIN));
    char buffer[4] = {}; CHECK(read(pipes[0], buffer, 2) == 2 && memcmp(buffer, "io", 2) == 0);
    CHECK(close(pipes[0]) == 0 && close(pipes[1]) == 0);
    int event = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC); REQUIRE(event >= 0);
    int ep = epoll_create1(EPOLL_CLOEXEC); REQUIRE(ep >= 0);
    epoll_event registration{}; registration.events = EPOLLIN; registration.data.u64 = 42;
    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, event, &registration) == 0);
    epoll_event received{}; CHECK(epoll_wait(ep, &received, 1, 0) == 0);
    eventfd_t number = 7; CHECK(eventfd_write(event, number) == 0);
    CHECK(epoll_wait(ep, &received, 1, 1000) == 1 && received.data.u64 == 42 && (received.events & EPOLLIN));
    CHECK(eventfd_read(event, &number) == 0 && number == 7);
    CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, event, nullptr) == 0 && close(ep) == 0 && close(event) == 0);
    int sockets[2]; REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    CHECK(send(sockets[0], "OK", 2, 0) == 2 && recv(sockets[1], buffer, 2, MSG_WAITALL) == 2 && memcmp(buffer, "OK", 2) == 0);
    CHECK(send(sockets[1], "io", 2, 0) == 2 && recv(sockets[0], buffer, 2, MSG_WAITALL) == 2 && memcmp(buffer, "io", 2) == 0);
    CHECK(close(sockets[0]) == 0 && close(sockets[1]) == 0);
    q.step("libc: pipe poll eventfd epoll socketpair OK");
    return q.finish();
}
