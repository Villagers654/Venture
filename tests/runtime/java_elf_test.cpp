#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>

#include "java/elf_exports.h"

namespace {
template <class T>
T read(const std::vector<char>& data, std::size_t offset) {
  if (offset > data.size() || sizeof(T) > data.size() - offset)
    throw std::runtime_error("bad test ELF bounds");
  T result;
  std::memcpy(&result, data.data() + offset, sizeof(result));
  return result;
}
struct Header {
  std::uint32_t type, flags;
  std::uint64_t offset, vaddr, paddr, filesz, memsz, align;
};
class MappedLibrary : public venture::Library {
 public:
  std::vector<char> mapping;
  std::map<std::string, std::uint64_t> expected;
  MappedLibrary(const std::filesystem::path& path, std::map<std::string, std::uint64_t> symbols)
      : expected(std::move(symbols)) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<char> file((std::istreambuf_iterator<char>(stream)), {});
    if (read<std::uint16_t>(file, 18) != 183) throw std::runtime_error("fixture is not AArch64");
    auto offset = read<std::uint64_t>(file, 32);
    auto count = read<std::uint16_t>(file, 56);
    std::uint64_t size = 0;
    for (unsigned i = 0; i < count; ++i) {
      auto p = read<Header>(file, offset + 56 * i);
      if (p.type == 1) size = std::max(size, p.vaddr + p.memsz);
    }
    if (size > 256 * 1024 * 1024) throw std::runtime_error("oversized fixture image");
    mapping.resize(size);
    for (unsigned i = 0; i < count; ++i) {
      auto p = read<Header>(file, offset + 56 * i);
      if (p.type == 1) {
        if (p.offset + p.filesz > file.size() || p.vaddr + p.filesz > mapping.size())
          throw std::runtime_error("bad fixture PT_LOAD");
        std::memcpy(mapping.data() + p.vaddr, file.data() + p.offset, p.filesz);
      }
    }
    load_bias = map_start = venture::h2g(mapping.data());
    map_size = mapping.size();
    phdr = map_start + offset;
    phnum = count;
  }
  venture::GuestAddr find_symbol(std::string_view name, std::string_view = {}) const override {
    auto it = expected.find(std::string(name));
    return it == expected.end() ? 0 : load_bias + it->second;
  }
};
}  // namespace
TEST_CASE("JNI export enumeration on owned Bait and Epic libgame ELF images") {
  const auto root = std::filesystem::path(VENTURE_TEST_NATIVE_FIXTURES);
  for (auto package : {"com.resolutiongames.baitsantacruz",
                       "games.b4t.epicrollercoasters.oculus"}) {
    auto file = root / (std::string(package) + ".json");
    if (!std::filesystem::exists(file)) {
      MESSAGE("Owned APK fixture absent: ", package, " (run tests/runtime/java_extract_owned.py)");
      continue;
    }
    std::ifstream json(file);
    nlohmann::json census;
    json >> census;
    MappedLibrary library(root / (std::string(package) + ".so"),
                          census.at("exports").get<std::map<std::string, std::uint64_t>>());
    std::map<std::string, std::uint64_t> actual;
    venture::java::exported_java_symbols(library, [&](std::string_view name, venture::GuestAddr address) {
      actual.emplace(name, address - library.load_bias);
    });
    CHECK(actual == library.expected);
    CHECK(actual.size() == 3);
    CHECK(census.at("onload").get<bool>() ==
          (std::string_view(package) == "games.b4t.epicrollercoasters.oculus"));
    auto size = library.map_size;
    library.map_size = 16;
    CHECK_THROWS(venture::java::exported_java_symbols(library, [](auto, auto) {}));
    library.map_size = size;
  }
}
