// The guest's view of an Android app process (Quest: Android 12L, API 32):
// /system/bin/app_process64, the bionic linker and the system libraries as ELF
// objects in memory and on the guest filesystem, the linker's soinfo list,
// r_debug for load notifications, and the auxiliary vector. In-process tools
// (instrumentation runtimes, crash reporters, mod loaders) locate the linker
// and libc through exactly these structures on a real device.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/guest.h"

namespace venture {
class GuestThread;
class Library;
struct DlInfo;
}

namespace venture::process_image {
// After HLE registration, before guest code runs. A64 processes only.
void publish();
bool published();
// Published ELF objects without native backing. Snapshot under the process
// image lock; callers may invoke guest callbacks after releasing that lock.
struct SyntheticModule {
  std::string path;
  GuestAddr load_bias, phdr;
  std::uint16_t phnum;
};
std::vector<SyntheticModule> synthetic_modules();
// The linker's current libraries in load order, under the loader lock. Calls
// the guest's r_brk hook (as bionic does) when someone has replaced it.
void sync(const std::vector<std::shared_ptr<Library>>& libraries);
// dladdr() for HLE entry points: the owning system library image.
bool address_info(GuestAddr address, DlInfo* out);
// Guest file contents of a synthesized image (/apex/.../libc.so, ...).
std::optional<std::string> file(std::string_view guest_path);
bool has_file(std::string_view guest_path);
// AT_* values of the process; false when the type is absent.
bool auxval(std::uint64_t type, std::uint64_t* value);
std::string auxv();
// bionic's thread list (libc g_thread_list, read by in-process tools under
// g_thread_list_lock): each pthread_t handle starts with pthread_internal_t's
// next, prev and tid. Called outside the thread registry lock.
void thread_attached(GuestThread& thread);
void thread_detached(GuestThread& thread);
// A new pthread enters through libc's __pthread_start, as on bionic: returns
// {entry, argument} for the thread's first guest call.
std::pair<GuestAddr, GuestAddr> thread_entry(GuestThread& thread, GuestAddr start, GuestAddr argument);
// Device locations: ld-android.so is the linker itself, bionic and ART live in APEXes.
std::string system_path(std::string_view soname);
std::string program_path();
}  // namespace venture::process_image
