#include "vtest.h"
#include <atomic>
#include <dlfcn.h>
#include <thread>

static __thread int local_number = 7;
static std::atomic<int> destroyed{0};
static thread_local int destruction_order = 0;
static std::atomic<bool> order_ok{true};
static void observe(int id) { destruction_order = destruction_order * 10 + id; }
struct Local {
    int value = 11;
    ~Local() { observe(1); if (destruction_order != 21) order_ok = false; destroyed.fetch_add(1); }
};
static thread_local Local object;
extern "C" int vtest_main(int, char**) {
    Vtest q("tls"); destroyed = 0; order_ok = true;
    CHECK(local_number == 7 && object.value == 11);
    // Start a thread before dlopen: the loader must populate late TLS for it too.
    std::atomic<bool> loaded{false}, worker_ready{false}, worker_ok{true};
    int (*peer_get)() = nullptr; int (*peer_object_get)() = nullptr;
    void (*peer_set)(int) = nullptr;
    std::thread existing([&] {
        if (local_number != 7 || object.value != 11) worker_ok = false;
        worker_ready.store(true, std::memory_order_release);
        while (!loaded.load(std::memory_order_acquire)) std::this_thread::yield();
        if (!peer_get) return;
        if (peer_get() != 17 || peer_object_get() != 23) worker_ok = false;
        local_number = 100; object.value = 101; peer_set(200);
        if (local_number != 100 || object.value != 101 || peer_get() != 200 || peer_object_get() != 201) worker_ok = false;
    });
    while (!worker_ready.load(std::memory_order_acquire)) std::this_thread::yield();
    void* lib = dlopen("libvtest_tls_peer.so", RTLD_NOW | RTLD_LOCAL);
    if (!CHECK(lib)) { loaded.store(true, std::memory_order_release); existing.join(); return q.finish(); }
    peer_get = reinterpret_cast<int (*)()>(dlsym(lib, "vtest_tls_peer_get"));
    peer_object_get = reinterpret_cast<int (*)()>(dlsym(lib, "vtest_tls_peer_object_get"));
    peer_set = reinterpret_cast<void (*)(int)>(dlsym(lib, "vtest_tls_peer_set"));
    auto peer_destroyed = reinterpret_cast<int (*)()>(dlsym(lib, "vtest_tls_peer_destroyed"));
    auto peer_observe = reinterpret_cast<void (*)(void (*)(int))>(dlsym(lib, "vtest_tls_peer_observe"));
    if (!CHECK(peer_get && peer_object_get && peer_set && peer_destroyed && peer_observe)) {
        peer_get = nullptr; loaded.store(true, std::memory_order_release); existing.join(); dlclose(lib); return q.finish();
    }
    peer_observe(observe);
    loaded.store(true, std::memory_order_release); existing.join();
    CHECK(worker_ok.load() && destroyed == 1 && peer_destroyed() == 1 && order_ok.load());
    q.step("TLS: existing thread late-loaded module initial values isolation OK");
    // New threads get fresh TLS for both DSOs. Touch local before peer to define
    // registration order; peer destructor must run first, then local destructor.
    std::thread fresh([&] {
        if (local_number != 7 || object.value != 11 || peer_get() != 17 || peer_object_get() != 23) worker_ok = false;
        local_number = 300; peer_set(400);
    });
    fresh.join();
    CHECK(worker_ok.load() && destroyed == 2 && peer_destroyed() == 2 && order_ok.load());
    CHECK(local_number == 7 && object.value == 11);
    q.step("TLS: new thread isolation reverse destructor order OK");
    // Do not touch peer TLS on the calling thread; dlclose can then unload it.
    CHECK(dlclose(lib) == 0);
    return q.finish();
}
