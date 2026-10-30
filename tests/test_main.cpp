#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <cstdio>
#include <cstdlib>
#if defined(_WIN32) && defined(VENTURE_TEST_CRASH_REPORT)
#include <filesystem>
#include "platform/crash_report.h"
#endif
#ifndef _WIN32
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>
#endif

int main(int argc, char** argv) {
  // Unbuffered, so a crashing test's name and failed checks reach the log.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(_WIN32) && defined(VENTURE_TEST_CRASH_REPORT)
  // A crashing test logs its faulting stack and leaves a dump beside the binary.
  venture::platform::install_crash_report(
      (std::filesystem::absolute(argv[0]).parent_path() / "venture_tests.log").string(), "venture_tests");
#endif
  // Decoded textures stay RGBA8 (the runtime stores them as BC7 by default);
  // the BC7 tests force compression themselves.
#ifdef _WIN32
  if (!std::getenv("VENTURE_TEXTURE_COMPRESSION")) _putenv_s("VENTURE_TEXTURE_COMPRESSION", "never");
#else
  setenv("VENTURE_TEXTURE_COMPRESSION", "never", 0);
#endif
#ifndef _WIN32
  // Fixture credentials must never reach the owner's Secret Service (DPAPI
  // blobs on Windows stay in each test's temporary directory). libsecret's file
  // backend keeps them in a private keyring that child processes inherit and
  // that is removed at exit.
  std::filesystem::path keyring;
  if (!std::getenv("SECRET_FILE_TEST_PATH")) {
    keyring = std::filesystem::temp_directory_path() / ("venture-test-keyring-" + std::to_string(getpid()));
    std::filesystem::create_directories(keyring);
    setenv("SECRET_BACKEND", "file", 1);
    setenv("SECRET_FILE_TEST_PATH", (keyring / "keyring").c_str(), 1);
    setenv("SECRET_FILE_TEST_PASSWORD", "venture-tests", 1);
  }
#endif
  const int result = doctest::Context(argc, argv).run();
#ifndef _WIN32
  if (!keyring.empty()) {
    std::error_code ignored;
    std::filesystem::remove_all(keyring, ignored);
  }
#endif
  return result;
}
