#include <doctest/doctest.h>
#include "common/log.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <cstdio>
#ifdef _WIN32
#include <io.h>
#define dup _dup
#define dup2 _dup2
#define fileno _fileno
#else
#include <unistd.h>
#endif

using namespace venture;
namespace {
struct LogFile {
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      std::format("venture-log-test-{}", std::chrono::steady_clock::now().time_since_epoch().count());
  LogFile() { log::set_file(path.string()); }
  ~LogFile() { log::set_file(""); std::error_code ignored; std::filesystem::remove(path, ignored); }
  std::string text() {
    log::set_file("");
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
  }
};
std::size_t count(const std::string& text, std::string_view needle) {
  std::size_t n = 0;
  for (auto at = text.find(needle); at != text.npos; at = text.find(needle, at + needle.size())) ++n;
  return n;
}
}
TEST_CASE("log lines never keep a Meta access token a game logs") {
  LogFile file;
  const std::string user = "FRLA" + std::string(40, 'x') + "ZD";
  const std::string app = "OC|5812519008825194|" + std::string(32, 'a');
  log::info("Unity", "Beginning login Token: <redacted> : {} and {}.", user, app);
  log::info("Unity", "FRL support and OC|12 and FRLshort stay as written");
  const auto text = file.text();
  CHECK(count(text, "Token: <redacted> : <redacted-token> and <redacted-token>.\n") == 1);
  CHECK(text.find(user) == std::string::npos);
  CHECK(text.find(app) == std::string::npos);
  CHECK(count(text, "FRL support and OC|12 and FRLshort stay as written\n") == 1);
}
