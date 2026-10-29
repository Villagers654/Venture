#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include <cstdio>
#ifndef _WIN32
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>
#endif

int main(int argc, char** argv) {
  // Unbuffered, so a crashing test's name and failed checks reach the log.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
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
