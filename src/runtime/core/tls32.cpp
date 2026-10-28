#include "core/tls32.h"
#include "core/address_space.h"
#include "core/thread.h"
#include "common/log.h"
#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <vector>

namespace venture::tls::a32 {
namespace {
struct Image {
  std::vector<std::byte> data;
  std::uint64_t size, offset;
};
struct Block {
  GuestAddr allocation, dtv;
};
std::mutex mutex;
std::vector<Image> modules;
std::map<GuestThread*, Block> blocks;
std::uint64_t used = 32;
std::uint32_t guard() {
  static const auto value = std::random_device{}() & ~0xffu;
  return value;
}
void init(GuestThread& t, const Block& b, const Image& image, std::size_t id) {
  auto addr = t.thread_pointer + image.offset;
  std::memset(g2h(addr), 0, image.size);
  std::memcpy(g2h(addr), image.data.data(), image.data.size());
  auto* dtv = g2h<std::uint32_t>(b.dtv);
  dtv[id] = h2g32(g2h(addr));
  dtv[0] = static_cast<std::uint32_t>(modules.size());
}
}  // namespace
Module add_module(std::span<const std::byte> data, std::uint64_t size, std::uint64_t alignment,
                  std::uint64_t skew) {
  std::lock_guard lock(mutex);
  alignment = std::max<std::uint64_t>(alignment, 1);
  if ((alignment & (alignment - 1)) || alignment > 4096 || skew >= alignment || data.size() > size ||
      modules.size() + 1 >= max_modules)
    throw std::runtime_error("unsupported A32 ELF TLS alignment/size/module count");
  auto offset = (used > skew ? align_up(used - skew, alignment) : 0) + skew;
  if (offset > surplus || size > surplus - offset)
    throw std::runtime_error("A32 64 KiB static TLS surplus exhausted");
  modules.push_back({{data.begin(), data.end()}, size, offset});
  used = offset + size;
  for (auto& [thread, block] : blocks) init(*thread, block, modules.back(), modules.size());
  log::info("tls", "A32 static TLS used {} / {} bytes, {} modules", used, surplus, modules.size());
  return {modules.size(), static_cast<std::int64_t>(offset)};
}
void attach(GuestThread& t) {
  std::lock_guard lock(mutex);
  if (blocks.contains(&t)) {
    // Reattached after a detach: the block stays, the new CPU needs the pointer.
    t.cpu().set_tpidr(t.thread_pointer);
    t.cpu().set_tpidrro(t.thread_pointer);
    return;
  }
  auto allocation = AddressSpace::get().allocate(allocation_size, kProtRead | kProtWrite, "[A32 TLS]");
  if (!allocation) throw std::bad_alloc();
  t.thread_pointer = allocation + 4096;
  Block block{allocation, allocation + 4096 + surplus + 8};
  auto* slots = g2h<std::uint32_t>(t.thread_pointer);
  // Prefix reserves bionic TLS storage independently of APP and the errno cell.
  slots[-1] = h2g32(g2h(allocation));
  slots[0] = h2g32(g2h(block.dtv));
  slots[1] = h2g32(g2h(t.pthread_self));
  slots[5] = guard();
  // bionic/libc/private/bionic_elf_tls.h: count, next precede generation.
  g2h<std::uint32_t>(block.dtv)[-2] = max_modules - 1;
  for (std::size_t i = 0; i < modules.size(); ++i) init(t, block, modules[i], i + 1);
  blocks.emplace(&t, block);
  t.cpu().set_tpidr(t.thread_pointer);
  t.cpu().set_tpidrro(t.thread_pointer);
}
void detach(GuestThread& t) {
  std::lock_guard lock(mutex);
  if (auto it = blocks.find(&t); it != blocks.end()) {
    AddressSpace::get().free(it->second.allocation);
    blocks.erase(it);
  }
}
GuestAddr get_addr(GuestThread& t, std::uint64_t module, std::uint64_t offset) {
  std::lock_guard lock(mutex);
  if (!module || module > modules.size() || offset >= modules[module - 1].size) return 0;
  return t.thread_pointer + modules[module - 1].offset + offset;
}
}  // namespace venture::tls::a32
