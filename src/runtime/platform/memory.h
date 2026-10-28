#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include "core/guest.h"

namespace venture::platform {
std::uint64_t reservation_granularity();
GuestAddr reserve(GuestAddr address, std::uint64_t size, bool exact);
void release(GuestAddr address, std::uint64_t size);
bool commit(GuestAddr address, std::uint64_t size, std::uint32_t prot);
bool decommit(GuestAddr address, std::uint64_t size);
bool protect(GuestAddr address, std::uint64_t size, std::uint32_t prot);
bool memory_accessible(GuestAddr address, std::size_t size, bool write);
bool read_memory(GuestAddr address, void* out, std::size_t size);
bool write_memory(GuestAddr address, const void* data, std::size_t size);
bool compare_exchange(GuestAddr address, const void* value, void* expected, std::size_t size);
// One locked host instruction on a naturally aligned 1/2/4/8-byte object, with
// no syscall. A fault (unmapped, read-only, or unmapped by a racing thread) is
// contained: the call returns false and memory is unchanged. `old` receives the
// prior value zero-extended. compare_exchange stores only when old == expected.
enum class AtomicRmw { add, exchange, compare_exchange };
bool atomic_rmw(GuestAddr address, unsigned size, AtomicRmw op, std::uint64_t value,
                std::uint64_t desired, std::uint64_t& old);
// True when a host instruction pointer lies in a fault-contained routine above;
// other fault handlers must leave those faults to the platform.
bool guarded_access_ip(std::uintptr_t ip);
// Fault-contained 32-bit load, e.g. re-reading a guest instruction.
bool guarded_load32(GuestAddr address, std::uint32_t& value);
// Fault-contained copy to guest memory without overriding page protections.
// A fault may leave a prefix written, as with Linux copy_to_user.
bool guarded_store(GuestAddr address, const void* data, std::size_t size);
enum class NativeMemoryOp { copy, move, fill, compare, find };
struct NativeMemoryFault { GuestAddr address{}; bool write{}, bus{}; };
// Recover only accesses to the declared guest ranges in these leaf CRT calls.
// Exceptions elsewhere remain host exceptions. A fault may leave a prefix copied.
bool native_memory(NativeMemoryOp, GuestAddr destination, GuestAddr source, std::size_t size,
                   int value, std::uint64_t& result, NativeMemoryFault& fault, bool source_guest = true);
// Typed hot-path leaves avoid the generic operation/result argument packet.
bool native_copy(GuestAddr destination, GuestAddr source, std::size_t, NativeMemoryFault&);
bool native_move(GuestAddr destination, GuestAddr source, std::size_t, NativeMemoryFault&);
bool native_fill(GuestAddr destination, int value, std::size_t, NativeMemoryFault&);
bool native_compare(GuestAddr a, GuestAddr b, std::size_t, int& result, NativeMemoryFault&);
// Windows commit (private or mapped view) in a range; nullopt on hosts without this
// accounting concept, or when the range cannot be queried. Diagnostics/tests.
std::optional<std::uint64_t> committed_bytes(GuestAddr address, std::uint64_t size);
struct FileMapping { GuestAddr base = 0, address = 0; std::uint64_t size = 0; };
int map_shared_file(int host_fd, std::uint64_t offset, std::uint64_t size, std::uint32_t prot, FileMapping& out);
void unmap_shared_file(const FileMapping& mapping);
int sync_shared_file(GuestAddr address, std::uint64_t size, bool synchronous);
}
