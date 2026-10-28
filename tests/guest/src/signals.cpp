#include "vtest.h"
#include <cerrno>
#include <csetjmp>
#include <csignal>
#include <cstdlib>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>

// IL2CPP/Boehm uses thread signals for suspension and synchronous fault recovery.
// Handlers use only async-signal-safe calls. No timers or SIGALRM are involved.
static volatile sig_atomic_t usr1_count, usr2_count, info_ok, on_alt, segv_ok, reply;
static uintptr_t alt_begin, alt_end;
static pthread_t calling_thread;
static sem_t reply_done, worker_release;
static sigjmp_buf recovery;
static void handler(int signal, siginfo_t* info, void*) {
    char marker;
    if (signal == SIGSEGV) {
        segv_ok = info && info->si_signo == SIGSEGV && info->si_addr == nullptr;
        siglongjmp(recovery, 1);
    }
    if (info && info->si_signo == signal && info->si_pid == getpid()) info_ok = 1;
    if (signal == SIGUSR1) {
        ++usr1_count;
        uintptr_t address = reinterpret_cast<uintptr_t>(&marker);
        if (address >= alt_begin && address < alt_end) on_alt = 1;
        if (reply) sem_post(&reply_done);
    } else {
        ++usr2_count;
        pthread_kill(calling_thread, SIGUSR1);
    }
}
static void* signal_worker(void*) {
    while (sem_wait(&worker_release) == -1 && errno == EINTR) {}
    return nullptr;
}
static __attribute__((noinline)) void fault_at_null() {
    // Inline assembly prevents optimizer removal of a deliberate null load.
    uintptr_t address = 0; unsigned value;
#if defined(__aarch64__)
    __asm__ volatile("ldr %w0, [%1]" : "=r"(value) : "r"(address) : "memory");
#else
    __asm__ volatile("ldr %0, [%1]" : "=r"(value) : "r"(address) : "memory");
#endif
    (void)value;
}
extern "C" int vtest_main(int, char**) {
    Vtest q("signals");
    usr1_count = usr2_count = info_ok = on_alt = segv_ok = reply = 0;
    calling_thread = pthread_self();
    sigset_t unblocked, old_mask; sigemptyset(&unblocked);
    sigaddset(&unblocked, SIGUSR1); sigaddset(&unblocked, SIGUSR2); sigaddset(&unblocked, SIGSEGV);
    REQUIRE(pthread_sigmask(SIG_UNBLOCK, &unblocked, &old_mask) == 0);
    void* memory = malloc(64 * 1024); REQUIRE(memory);
    alt_begin = reinterpret_cast<uintptr_t>(memory); alt_end = alt_begin + 64 * 1024;
    stack_t stack{}; stack.ss_sp = memory; stack.ss_size = 64 * 1024;
    stack_t old_stack; REQUIRE(sigaltstack(&stack, &old_stack) == 0);
    struct sigaction action{}, old_usr1{}, old_usr2{}, old_segv{};
    action.sa_sigaction = handler; action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    REQUIRE(sigaction(SIGUSR1, &action, &old_usr1) == 0 && sigaction(SIGUSR2, &action, &old_usr2) == 0);
    REQUIRE(sigaction(SIGSEGV, &action, &old_segv) == 0);
    CHECK(raise(SIGUSR1) == 0 && usr1_count == 1 && info_ok && on_alt);
    q.step("signal: raise SA_SIGINFO alternate stack OK");
    sigset_t blocked, pending; sigemptyset(&blocked); sigaddset(&blocked, SIGUSR1);
    CHECK(sigprocmask(SIG_BLOCK, &blocked, nullptr) == 0);
    CHECK(raise(SIGUSR1) == 0 && usr1_count == 1);
    CHECK(sigpending(&pending) == 0 && sigismember(&pending, SIGUSR1) == 1);
    CHECK(pthread_sigmask(SIG_UNBLOCK, &blocked, nullptr) == 0 && usr1_count == 2);
    q.step("signal: process and pthread masks pending delivery OK");
    REQUIRE(sem_init(&reply_done, 0, 0) == 0 && sem_init(&worker_release, 0, 0) == 0);
    pthread_t worker; REQUIRE(pthread_create(&worker, nullptr, signal_worker, nullptr) == 0);
    reply = 1; CHECK(pthread_kill(worker, SIGUSR2) == 0);
    timespec deadline = vtest_deadline(); int result;
    do { result = sem_timedwait(&reply_done, &deadline); } while (result == -1 && errno == EINTR);
    CHECK(result == 0); reply = 0;
    CHECK(sem_post(&worker_release) == 0 && pthread_join(worker, nullptr) == 0);
    CHECK(usr2_count == 1 && usr1_count == 3);
    CHECK(sem_destroy(&reply_done) == 0 && sem_destroy(&worker_release) == 0);
    q.step("signal: pthread_kill worker and reply OK");
    if (sigsetjmp(recovery, 1) == 0) { fault_at_null(); q.check(false, "null load did not raise SIGSEGV"); }
    CHECK(segv_ok);
    q.step("signal: null fault SIGSEGV siglongjmp recovery OK");
    CHECK(sigaction(SIGUSR1, &old_usr1, nullptr) == 0 && sigaction(SIGUSR2, &old_usr2, nullptr) == 0);
    CHECK(sigaction(SIGSEGV, &old_segv, nullptr) == 0);
    CHECK(sigaltstack(&old_stack, nullptr) == 0); free(memory);
    CHECK(pthread_sigmask(SIG_SETMASK, &old_mask, nullptr) == 0);
    return q.finish();
}
