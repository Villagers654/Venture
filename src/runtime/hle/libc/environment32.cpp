#include "hle/libc/libc_a.h"
#include "core/address_space.h"
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace venture::hle::libc {
namespace {
std::mutex mutex;
GuestStorage cell;
// getenv and replaced environ arrays may still be in use on other threads.
// putenv retains the caller's exact address, including subsequent mutations.
std::vector<std::unique_ptr<GuestStorage>> retained;
std::uint32_t* environ_cell() { return g2h<std::uint32_t>(cell.resize(4)); }
std::vector<std::uint32_t> entries() {
  std::vector<std::uint32_t> out;
  if (auto p = *environ_cell())
    for (auto* a = g2h<std::uint32_t>(p); *a; ++a) out.push_back(*a);
  return out;
}
bool matches(std::uint32_t p, const char* name, std::size_t n) {
  auto* text = g2h<const char>(p);
  return !std::strncmp(text, name, n) && text[n] == '=';
}
void publish(const std::vector<std::uint32_t>& values) {
  auto storage = std::make_unique<GuestStorage>();
  auto p = storage->resize((values.size() + 1) * 4);
  std::memcpy(g2h(p), values.data(), values.size() * 4);
  g2h<std::uint32_t>(p)[values.size()] = 0;
  retained.push_back(std::move(storage));
  *environ_cell() = h2g32(g2h(p));
}
bool valid_name(const char* name) { return name && *name && !std::strchr(name, '='); }
int unset(const char* name) {
  if (!valid_name(name)) {
    set_error(err::kEINVAL);
    return -1;
  }
  std::lock_guard lock(mutex);
  auto values = entries();
  auto n = std::strlen(name);
  std::erase_if(values, [&](auto p) { return matches(p, name, n); });
  publish(values);
  return 0;
}
void replace(std::vector<std::uint32_t>& values, const char* name, std::size_t n, std::uint32_t text) {
  bool found = false;
  for (auto& p : values)
    if (matches(p, name, n)) {
      p = text;
      found = true;
    }
  if (!found) values.push_back(text);
  publish(values);
}
int put(char* text) {
  if (!text) {
    set_error(err::kEINVAL);
    return -1;
  }
  auto* eq = std::strchr(text, '=');
  if (!eq) return unset(text);
  if (eq == text) {
    set_error(err::kEINVAL);
    return -1;
  }
  std::lock_guard lock(mutex);
  auto values = entries();
  replace(values, text, eq - text, h2g32(text));
  return 0;
}
int set(const char* name, const char* value, int overwrite) {
  if (!valid_name(name) || !value) {
    set_error(err::kEINVAL);
    return -1;
  }
  std::lock_guard lock(mutex);
  auto values = entries();
  auto n = std::strlen(name);
  for (auto p : values)
    if (matches(p, name, n) && !overwrite) return 0;
  auto storage = std::make_unique<GuestStorage>();
  auto* text = storage->string(std::string(name) + "=" + value);
  retained.push_back(std::move(storage));
  replace(values, name, n, h2g32(text));
  return 0;
}
int clear() {
  std::lock_guard lock(mutex);
  publish({});
  return 0;
}
}  // namespace
const char* getenv32(const char* name) {
  if (!valid_name(name)) return nullptr;
  std::lock_guard lock(mutex);
  auto n = std::strlen(name);
  for (auto p : entries())
    if (matches(p, name, n)) return g2h<const char>(p) + n + 1;
  return nullptr;
}
int setenv32(const char* name, const char* value, int overwrite) {
  return set(name, value, overwrite);
}
void register_environment32(HleLibrary& lib) {
  clear();
  lib.add_data("environ", h2g(environ_cell()));
  bind<&getenv32>(lib, "getenv");
  bind<&getenv32>(lib, "secure_getenv");
  bind<&set>(lib, "setenv");
  bind<&unset>(lib, "unsetenv");
  bind<&put>(lib, "putenv");
  bind<&clear>(lib, "clearenv");
}
}  // namespace venture::hle::libc
