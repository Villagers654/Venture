#include "common/environment.h"
#include <doctest/doctest.h>
#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
namespace {
void set(const char* key, const char* value) {
#ifdef _WIN32
  _putenv_s(key, value ? value : "");
  // Unlike _putenv_s, the process environment can represent an explicitly empty value.
  SetEnvironmentVariableA(key, value);
#else
  if (value) setenv(key,value,1); else unsetenv(key);
#endif
}
struct Fixture {
  const char* key = "VENTURE_ENV_TEST_VALUE";
  Fixture() { set(key,nullptr); venture::environment::refresh(); }
  ~Fixture() { set(key,nullptr); venture::environment::refresh(); }
};
}
TEST_CASE("host environment concurrent readers survive snapshot refresh") {
  Fixture f; set(f.key,"first"); venture::environment::refresh();
  const char* first = venture::environment::getenv(f.key);
  std::atomic<bool> go{false}, bad{false};
  std::vector<std::thread> readers;
  for (int i=0;i<16;++i) readers.emplace_back([&]{
    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
    for (int j=0;j<10000;++j) {
      auto* p=venture::environment::getenv(f.key);
      if (!p || (std::string_view(p)!="first" && std::string_view(p)!="second")) bad=true;
      if (venture::environment::getenv("VENTURE_ENV_MISSING_TEST")) bad=true;
    }
  });
  go.store(true,std::memory_order_release);
  for (int i=0;i<20;++i) { set(f.key,i%2 ? "first":"second"); venture::environment::refresh(); }
  for (auto& thread:readers) thread.join();
  CHECK_FALSE(bad.load());
  CHECK(std::string_view(first)=="first");
}
#ifdef _WIN32
TEST_CASE("host environment values are UTF-8 whatever the ANSI code page") {
  Fixture f;
  // A Turkish user folder: S-cedilla and u-umlaut are outside most ANSI code pages' ASCII.
  SetEnvironmentVariableW(L"VENTURE_ENV_TEST_VALUE", L"C:\\Users\\\u015e\u00fckr\u00fc\\bin");
  venture::environment::refresh();
  const char* value = venture::environment::getenv(f.key);
  REQUIRE(value);
  CHECK(std::string_view(value) == "C:\\Users\\\xC5\x9E\xC3\xBCkr\xC3\xBC\\bin");
}
#endif
