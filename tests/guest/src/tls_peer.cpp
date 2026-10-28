#include <atomic>
// Force late-loaded module TLS and nontrivial per-thread cleanup.
static __thread int peer_number = 17;
static std::atomic<int> destroyed{0};
static void (*observer)(int) = nullptr;
struct PeerLocal {
    int value = 23;
    ~PeerLocal() { destroyed.fetch_add(1); if (observer) observer(2); }
};
static thread_local PeerLocal peer_object;
extern "C" int vtest_tls_peer_get() { return peer_number; }
extern "C" int vtest_tls_peer_object_get() { return peer_object.value; }
extern "C" void vtest_tls_peer_set(int value) { peer_number = value; peer_object.value = value + 1; }
extern "C" int vtest_tls_peer_destroyed() { return destroyed.load(); }
extern "C" void vtest_tls_peer_observe(void (*callback)(int)) { observer = callback; }
