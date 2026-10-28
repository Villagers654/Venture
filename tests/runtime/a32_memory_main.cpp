#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>
#include "core/address_space.h"
#include "core/process.h"
#include "platform/memory.h"
#include <cstring>
#include <iostream>

int main(int argc, char** argv) {
  venture::process_config().abi = "armeabi-v7a";
  try {
    if (argc == 2 && std::strcmp(argv[1], "--reject-host-writable") == 0) {
      constexpr venture::GuestAddr p = 0x40000000;
      if (venture::platform::reserve(p, 65536, true) != p ||
          !venture::platform::commit(p, 4096, venture::kProtRead | venture::kProtWrite))
        return 2;
      try {
        venture::AddressSpace::get().initialize(venture::GuestIsa::a32);
      } catch (const std::runtime_error& e) {
        std::cout << e.what() << '\n';
        if (!std::strstr(e.what(), "host-owned writable")) return 3;
        try {
          venture::AddressSpace::get().allocate(4096, venture::kProtRead, "must fail");
        } catch (const std::runtime_error&) {
          return 0;
        }
        return 4;  // no fallback after admission failure
      }
      return 1;
    }
    venture::AddressSpace::get().initialize(venture::GuestIsa::a32);
    return doctest::Context(argc, argv).run();
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
