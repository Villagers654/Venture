#include "vtest.h"
#include <atomic>
#include <cerrno>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <sys/prctl.h>
#include <unistd.h>

// Engine job queues and GC rendezvous depend on bionic object layouts and waits.
static pthread_once_t once_control = PTHREAD_ONCE_INIT;
static std::atomic<int> once_calls{0}, key_destructions{0};
static void once_init() { ++once_calls; }
static void key_destroy(void* value) { if (value == reinterpret_cast<void*>(uintptr_t(42))) ++key_destructions; }
struct Group {
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
    pthread_barrier_t barrier{};
    pthread_key_t key{};
    bool release = false;
    int process_id = 0, main_tid = 0;
    int ready = 0;
    std::atomic<bool> ok{true};
    std::atomic<int> serial{0};
};
static void* group_worker(void* opaque) {
    auto& g = *static_cast<Group*>(opaque);
    if (!pthread_equal(pthread_self(), pthread_self()) || gettid() <= 0 || gettid() == g.main_tid || getpid() != g.process_id) g.ok = false;
    if (pthread_setname_np(pthread_self(), "vtest-worker") != 0) g.ok = false;
    char name[16] = {};
#if __ANDROID_API__ >= 26
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) != 0 || strcmp(name, "vtest-worker") != 0) g.ok = false;
#else
    if (prctl(PR_GET_NAME, name, 0, 0, 0) != 0 || strcmp(name, "vtest-worker") != 0) g.ok = false;
#endif
    if (pthread_once(&once_control, once_init) || pthread_setspecific(g.key, reinterpret_cast<void*>(uintptr_t(42)))) g.ok = false;
    if (pthread_getspecific(g.key) != reinterpret_cast<void*>(uintptr_t(42))) g.ok = false;
    pthread_mutex_lock(&g.mutex); ++g.ready; pthread_cond_broadcast(&g.cond);
    while (!g.release) pthread_cond_wait(&g.cond, &g.mutex);
    pthread_mutex_unlock(&g.mutex);
#if __ANDROID_API__ >= 24
    for (int i = 0; i < 2; ++i) {
        int result = pthread_barrier_wait(&g.barrier);
        if (result == PTHREAD_BARRIER_SERIAL_THREAD) ++g.serial;
        else if (result != 0) g.ok = false;
    }
#endif
    if (sched_yield() != 0) g.ok = false;
    return reinterpret_cast<void*>(uintptr_t(99));
}
struct Detached { sem_t gate{}, done{}; };
// The harness drains all guest threads before unloading test code. Keep these
// objects alive until then: observing sem_post is not a detached-thread join.
static Detached detached;
static void* detached_worker(void* opaque) {
    auto& d = *static_cast<Detached*>(opaque);
    while (sem_wait(&d.gate) < 0 && errno == EINTR) {}
    sem_post(&d.done); return nullptr;
}
extern "C" int vtest_main(int, char**) {
    Vtest q("pthreads");
    pthread_mutexattr_t ma; REQUIRE(pthread_mutexattr_init(&ma) == 0);
    for (int type : {PTHREAD_MUTEX_NORMAL, PTHREAD_MUTEX_RECURSIVE, PTHREAD_MUTEX_ERRORCHECK}) {
        pthread_mutex_t mutex;
        REQUIRE(pthread_mutexattr_settype(&ma, type) == 0 && pthread_mutex_init(&mutex, &ma) == 0);
        CHECK(pthread_mutex_lock(&mutex) == 0);
        if (type == PTHREAD_MUTEX_RECURSIVE) {
            CHECK(pthread_mutex_trylock(&mutex) == 0); CHECK(pthread_mutex_unlock(&mutex) == 0);
        } else {
            CHECK(pthread_mutex_trylock(&mutex) == EBUSY);
            if (type == PTHREAD_MUTEX_ERRORCHECK) CHECK(pthread_mutex_lock(&mutex) == EDEADLK);
            else { timespec past{0, 0}; CHECK(pthread_mutex_timedlock(&mutex, &past) == ETIMEDOUT); }
        }
        CHECK(pthread_mutex_unlock(&mutex) == 0);
        timespec future = vtest_deadline(); CHECK(pthread_mutex_timedlock(&mutex, &future) == 0);
        CHECK(pthread_mutex_unlock(&mutex) == 0 && pthread_mutex_destroy(&mutex) == 0);
    }
    CHECK(pthread_mutexattr_destroy(&ma) == 0);
    q.step("pthread: normal recursive errorcheck trylock timedlock OK");
    Group group;
    group.process_id = getpid(); group.main_tid = gettid();
    CHECK(group.process_id > 0 && group.main_tid > 0);
    once_control = PTHREAD_ONCE_INIT; once_calls = 0; key_destructions = 0;
    REQUIRE(pthread_key_create(&group.key, key_destroy) == 0);
#if __ANDROID_API__ >= 24
    REQUIRE(pthread_barrier_init(&group.barrier, nullptr, 3) == 0);
#endif
    pthread_attr_t attr; REQUIRE(pthread_attr_init(&attr) == 0);
    REQUIRE(pthread_attr_setstacksize(&attr, 1024 * 1024) == 0);
    size_t stacksize = 0; CHECK(pthread_attr_getstacksize(&attr, &stacksize) == 0 && stacksize >= 1024 * 1024);
    pthread_t threads[2];
    for (auto& t : threads) REQUIRE(pthread_create(&t, &attr, group_worker, &group) == 0);
    CHECK(!pthread_equal(threads[0], threads[1]));
    pthread_mutex_lock(&group.mutex);
    timespec deadline = vtest_deadline();
    while (group.ready != 2) {
        int result = pthread_cond_timedwait(&group.cond, &group.mutex, &deadline);
        if (result != 0) { q.check(false, "condition readiness wait failed"); break; }
    }
    group.release = true; pthread_cond_broadcast(&group.cond); pthread_mutex_unlock(&group.mutex);
#if __ANDROID_API__ >= 24
    for (int i = 0; i < 2; ++i) {
        int result = pthread_barrier_wait(&group.barrier);
        if (result == PTHREAD_BARRIER_SERIAL_THREAD) ++group.serial;
        else CHECK(result == 0);
    }
#endif
    for (auto t : threads) { void* result = nullptr; CHECK(pthread_join(t, &result) == 0 && result == reinterpret_cast<void*>(uintptr_t(99))); }
    CHECK(group.ok.load() && once_calls == 1 && key_destructions == 2);
#if __ANDROID_API__ >= 24
    CHECK(group.serial == 2);
#endif
    pthread_mutex_lock(&group.mutex); timespec past{0, 0};
    int wait_result;
    do { wait_result = pthread_cond_timedwait(&group.cond, &group.mutex, &past); } while (wait_result == 0);
    CHECK(wait_result == ETIMEDOUT);
    pthread_mutex_unlock(&group.mutex);
#if __ANDROID_API__ >= 24
    CHECK(pthread_barrier_destroy(&group.barrier) == 0);
#endif
    CHECK(pthread_key_delete(group.key) == 0);
    CHECK(pthread_cond_destroy(&group.cond) == 0 && pthread_mutex_destroy(&group.mutex) == 0);
    CHECK(pthread_attr_destroy(&attr) == 0);
    q.step("pthread: create join cond broadcast barrier once keys names stack OK");
    pthread_rwlock_t rw; REQUIRE(pthread_rwlock_init(&rw, nullptr) == 0);
    CHECK(pthread_rwlock_rdlock(&rw) == 0 && pthread_rwlock_tryrdlock(&rw) == 0);
    CHECK(pthread_rwlock_unlock(&rw) == 0 && pthread_rwlock_unlock(&rw) == 0);
    CHECK(pthread_rwlock_wrlock(&rw) == 0 && pthread_rwlock_tryrdlock(&rw) == EBUSY);
    CHECK(pthread_rwlock_unlock(&rw) == 0 && pthread_rwlock_trywrlock(&rw) == 0);
    CHECK(pthread_rwlock_unlock(&rw) == 0 && pthread_rwlock_destroy(&rw) == 0);
    REQUIRE(sem_init(&detached.gate, 0, 0) == 0 && sem_init(&detached.done, 0, 0) == 0);
    errno = 0; CHECK(sem_trywait(&detached.done) == -1 && errno == EAGAIN);
    errno = 0; CHECK(sem_timedwait(&detached.done, &past) == -1 && errno == ETIMEDOUT);
    pthread_t thread; REQUIRE(pthread_create(&thread, nullptr, detached_worker, &detached) == 0);
    CHECK(pthread_detach(thread) == 0 && sem_post(&detached.gate) == 0);
    deadline = vtest_deadline(); CHECK(sem_timedwait(&detached.done, &deadline) == 0);
    int count = -1; CHECK(sem_getvalue(&detached.done, &count) == 0 && count == 0);
    // A separate semaphore exercises explicit teardown without racing the last
    // instructions of detached_worker's sem_post.
    sem_t local_sem; REQUIRE(sem_init(&local_sem, 0, 1) == 0);
    CHECK(sem_wait(&local_sem) == 0 && sem_post(&local_sem) == 0 && sem_destroy(&local_sem) == 0);
    q.step("pthread: rwlock detached thread semaphores OK");
    return q.finish();
}
