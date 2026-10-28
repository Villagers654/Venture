#include "core/tls.h"
#include "core/address_space.h"
#include "core/hle_runtime.h"
#include "core/thread.h"
#include "core/tls32.h"
#include "core/process.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>
#include <vector>

namespace venture::tls {
namespace {
// bionic/libc/private/bionic_tls.h: AArch64 variant I, slots -1 through 7.
constexpr std::uint64_t surplus = 16 * 1024 * 1024;
constexpr std::uint64_t max_modules = 4096;
constexpr std::uint64_t tp_alignment = 1024 * 1024;
struct Image { std::vector<std::byte> data; std::uint64_t size, offset; };
struct Block { GuestAddr allocation, dtv; std::uint64_t committed = 0; };
std::mutex mutex;
std::vector<Image> modules;
std::map<GuestThread*, Block> threads;
std::uint64_t used = 8 * 8;
const std::uint64_t stack_guard = [] {
  std::random_device random;
  return ((static_cast<std::uint64_t>(random()) << 32) ^ random()) & ~std::uint64_t{0xff};
}();
void commit(Block& block, std::uint64_t size) {
  // Keep the prefix reachable through slot -1, and all existing TLS offsets.
  // Surplus is reserved address capacity; late modules commit it before use.
  const auto end = tp_alignment + align_up(size, kGuestPageSize);
  if (end <= block.committed) return;
  if (AddressSpace::get().protect(block.allocation + block.committed,
                                  end - block.committed, kProtRead | kProtWrite))
    throw std::bad_alloc();
  block.committed = end;
}
void init(GuestThread& thread, Block& block, const Image& image, std::size_t id) {
  std::memset(g2h(thread.thread_pointer + image.offset), 0, image.size);
  std::memcpy(g2h(thread.thread_pointer + image.offset), image.data.data(), image.data.size());
  // bionic/libc/private/bionic_elf_tls.h: dtv[0] = generation, dtv[module] = block.
  g2h<std::uint64_t>(block.dtv)[id] = thread.thread_pointer + image.offset;
  g2h<std::uint64_t>(block.dtv)[0] = modules.size();
}
}
Module add_module(std::span<const std::byte> data, std::uint64_t size, std::uint64_t alignment, std::uint64_t skew) {
  if (process_config().isa() == GuestIsa::a32) return a32::add_module(data, size, alignment, skew);
  std::lock_guard lock(mutex);
  alignment = std::max<std::uint64_t>(alignment, 1);
  if ((alignment & (alignment - 1)) || alignment > tp_alignment || skew >= alignment || data.size() > size || modules.size() + 1 >= max_modules)
    throw std::runtime_error("unsupported ELF TLS alignment/size/module count");
  // bionic TlsAlign preserves p_vaddr % p_align as well as the segment alignment.
  auto offset = (used > skew ? align_up(used - skew, alignment) : 0) + skew;
  if (offset > surplus || size > surplus - offset) throw std::runtime_error("static TLS surplus exhausted");
  // Grow every existing block before publishing the module. On failure the
  // module list/offsets remain unchanged; any extra committed pages are harmless.
  for (auto& [thread, block] : threads) commit(block, offset + size);
  modules.push_back({{data.begin(), data.end()}, size, offset});
  used = offset + size;
  for (auto& [thread, block] : threads) init(*thread, block, modules.back(), modules.size());
  return {modules.size(), static_cast<std::int64_t>(offset)};
}
void attach(GuestThread& thread) {
  if (process_config().isa() == GuestIsa::a32) {
    a32::attach(thread);
    return;
  }
  std::lock_guard lock(mutex);
  if (threads.contains(&thread)) {
    // Reattached after a detach: the block stays, the new CPU needs the pointer.
    thread.cpu().set_tpidr(thread.thread_pointer);
    return;
  }
  auto allocation = AddressSpace::get().allocate(surplus + tp_alignment, kProtNone, "[tls]", tp_alignment);
  auto dtv_allocation = AddressSpace::get().allocate((max_modules + 2) * 8, kProtRead | kProtWrite, "[dtv]");
  if (!allocation || !dtv_allocation) { AddressSpace::get().free(allocation); AddressSpace::get().free(dtv_allocation); throw std::bad_alloc(); }
  Block block{allocation, dtv_allocation + 16};
  try { commit(block, used); }
  catch (...) { AddressSpace::get().free(allocation); AddressSpace::get().free(dtv_allocation); throw; }
  // TlsDtv has count and next preceding the generation field to which slot 0 points.
  auto dtv = dtv_allocation + 16;
  g2h<std::uint64_t>(dtv)[-2] = max_modules - 1;
  thread.thread_pointer = allocation + tp_alignment;
  auto* slots = g2h<std::uint64_t>(thread.thread_pointer);
  slots[-1] = allocation; slots[0] = dtv; slots[1] = thread.pthread_self;
  // bionic/platform/bionic/tls_defines.h: slot 2 is TLS_SLOT_APP since API 29;
  // slot 5 is the AArch64 compiler's stack-protector guard, not an errno pointer.
  slots[5] = stack_guard;
  for (std::size_t i = 0; i < modules.size(); ++i) init(thread, block, modules[i], i + 1);
  threads.emplace(&thread, block);
  thread.cpu().set_tpidr(thread.thread_pointer);
}
void detach(GuestThread& thread) {
  if (process_config().isa() == GuestIsa::a32) {
    a32::detach(thread);
    return;
  }
  std::lock_guard lock(mutex);
  if (auto it = threads.find(&thread); it != threads.end()) {
    AddressSpace::get().free(it->second.allocation); AddressSpace::get().free(it->second.dtv - 16); threads.erase(it);
  }
}
GuestAddr get_addr(GuestThread& thread, std::uint64_t module, std::uint64_t offset) {
  if (process_config().isa() == GuestIsa::a32) return a32::get_addr(thread, module, offset);
  std::lock_guard lock(mutex);
  if (!module || module > modules.size() || offset >= modules[module - 1].size) return 0;
  return thread.thread_pointer + modules[module - 1].offset + offset;
}
GuestAddr descriptor_resolver() {
  static auto p = hle::code_stub(GuestStub::tlsdesc, "tlsdesc");
  return p;
}
GuestAddr get_addr_thunk() {
  static auto p = hle::make_thunk("libc.so", "__tls_get_addr", [](hle::CallContext& ctx) {
    auto* index = g2h<const std::uint64_t>(ctx.x(0));
    ctx.return_x(get_addr(ctx.thread(), index[0], index[1]));
  });
  return p;
}
GuestAddr weak_descriptor_resolver() {
  // bionic/linker/arch/arm64/tlsdesc_resolver.S: unresolved weak TLS yields NULL.
  // Preserve all registers except x0 and return addend - TP; the caller adds TP.
  static auto p = hle::code_stub(GuestStub::tlsdesc_weak, "tlsdesc weak");
  return p;
}
}
