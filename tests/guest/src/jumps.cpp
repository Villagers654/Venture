#include "vtest.h"
#include <csetjmp>
#include <csignal>
// Coroutines and IL2CPP fault recovery save guest register/stack state.
static __attribute__((noinline)) void jump(jmp_buf target) { longjmp(target, 42); }
static __attribute__((noinline)) void sigjump(sigjmp_buf target) { siglongjmp(target, 17); }
extern "C" int vtest_main(int, char**) {
    Vtest q("jumps"); jmp_buf context;
    volatile int visited = 0;
    int result = setjmp(context);
    if (result == 0) { visited = 1; jump(context); }
    CHECK(result == 42 && visited == 1);
    q.step("jump: setjmp longjmp register stack restore OK");
    sigset_t old, mask; sigemptyset(&mask); sigaddset(&mask, SIGUSR2);
    REQUIRE(sigprocmask(SIG_BLOCK, &mask, &old) == 0);
    sigjmp_buf signal_context;
    result = sigsetjmp(signal_context, 1);
    if (result == 0) { sigprocmask(SIG_UNBLOCK, &mask, nullptr); sigjump(signal_context); }
    sigset_t current; CHECK(sigprocmask(SIG_SETMASK, nullptr, &current) == 0);
    CHECK(result == 17 && sigismember(&current, SIGUSR2) == 1);
    CHECK(sigprocmask(SIG_SETMASK, &old, nullptr) == 0);
    q.step("jump: sigsetjmp siglongjmp signal mask restore OK");
    return q.finish();
}
