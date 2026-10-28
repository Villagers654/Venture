#include "vtest.h"
#include "cpp_peer.h"
#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <unordered_map>
#include <vector>
#include <atomic>

#ifndef VTEST_CPP_NAME
#define VTEST_CPP_NAME "cpp"
#endif
// Unreal plugins and IL2CPP native plugins exchange polymorphic objects and
// unwind through guest frames. Build this twice: static and shared libc++.
static void caller_throw() { throw std::runtime_error("caller exception"); }
static std::atomic<int> tls_destroyed{0};
struct Local {
    std::string text = "thread-local";
    ~Local() { if (text == "thread-local") tls_destroyed.fetch_add(1); }
};
static thread_local Local local;
extern "C" int vtest_main(int, char**) {
    Vtest q(VTEST_CPP_NAME);
    try {
        std::exception_ptr captured;
        try { vtest_cpp_throw(); } catch (const VtestError& e) {
            CHECK(strcmp(e.what(), "peer exception") == 0); captured = std::current_exception();
        }
        REQUIRE(captured != nullptr && vtest_cpp_unwound() == 1);
        bool caught = false;
        try { std::rethrow_exception(captured); } catch (const VtestError&) { caught = true; }
        CHECK(caught && vtest_cpp_catch(caller_throw));
        VtestBase* base = vtest_cpp_make(); REQUIRE(base);
        CHECK(dynamic_cast<VtestDerived*>(base) != nullptr && base->value() == 42);
        CHECK(typeid(*base) == typeid(VtestDerived)); vtest_cpp_delete(base);
        q.step("C++: cross-library exceptions exception_ptr RTTI OK");
        std::string text("ven"); text += "ture"; text.replace(0, 3, "Ven");
        CHECK(text == "Venture" && text.substr(3) == "ture");
        std::vector<int> vector{1, 2, 3}; vector.reserve(100); vector.push_back(4);
        int sum = 0; for (int item : vector) sum += item; CHECK(sum == 10);
        std::map<std::string, int> map{{"b", 2}, {"a", 1}};
        CHECK(map.begin()->first == "a" && map.at("b") == 2);
        std::unordered_map<std::string, int> hash; hash["engine"] = 42;
        CHECK(hash.at("engine") == 42 && hash.count("missing") == 0);
        q.step("C++: string vector map unordered_map OK");
        tls_destroyed = 0;
        std::mutex mutex; std::condition_variable cv;
        bool ready = false; int message = 0;
        std::promise<int> promise; auto future = promise.get_future();
        std::thread worker([&] {
            bool valid = local.text == "thread-local";
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return ready; });
            int result = message + (valid ? 1 : 0); lock.unlock(); promise.set_value(result);
        });
        { std::lock_guard<std::mutex> lock(mutex); message = 41; ready = true; }
        cv.notify_one();
        CHECK(future.get() == 42); worker.join(); CHECK(tls_destroyed == 1);
        q.step("C++: thread mutex condition future TLS destructor OK");
        auto start = std::chrono::steady_clock::now();
        auto end = std::chrono::steady_clock::now(); CHECK(end >= start);
        CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::seconds(2)).count() == 2000);
        CHECK(std::chrono::system_clock::to_time_t(std::chrono::system_clock::from_time_t(123456)) == 123456);
        q.step("C++: chrono OK");
    } catch (...) { q.check(false, "unexpected C++ exception"); }
    return q.finish();
}
