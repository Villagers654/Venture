#include "vfs.h"
#include "core/address_space.h"
#include "core/process_image.h"
#include "core/thread_api.h"
#include "platform/process_info.h"
#include "common/log.h"
#include <format>
#include <cstring>

namespace venture::hle::libc {
bool proc_directory(std::string_view p, std::vector<Dirent>* out) {
  std::vector<std::pair<std::string, int>> names;
  if (p == "/")
    names = {{"data", 4}, {"sdcard", 4}, {"storage", 4}, {"proc", 4},
             {"sys", 4},  {"dev", 4},    {"system", 4}, {"mnt", 4}};
  // These mount parents exist on Android independently of app directories.
  // Recursive directory creation reaches external storage through them.
  else if (p == "/storage")
    names = {{"emulated", 4}, {"self", 4}};
  else if (p == "/storage/emulated")
    names = {{"0", 4}};
  else if (p == "/storage/self")
    names = {{"primary", 4}};
  else if (p == "/mnt")
    names = {{"sdcard", 4}};
  else if (p == "/data")
    names = {{"user", 4}, {"data", 4}, {"app", 4}};
  else if (p == "/data/user")
    names = {{"0", 4}};
  else if (p == "/data/user/0" || p == "/data/data" || p == "/data/app")
    names = {{process_config().package, 4}};
  else if (p == "/proc")
    names = {{"self", 4}, {"cpuinfo", 8}, {"meminfo", 8}, {"stat", 8}};
  else if (p == "/proc/self")
    names = {{"maps", 8}, {"status", 8}, {"cmdline", 8}, {"exe", 10},  {"fd", 4},
             {"task", 4}, {"stat", 8},   {"auxv", 8},    {"cgroup", 8}};
  else if (p == "/proc/self/fd") {
    for (auto& [fd, path] : FdTable::get().paths()) names.emplace_back(std::to_string(fd), 10);
  } else if (p == "/proc/self/task") {
    for (auto& [tid, name] : threads::list()) names.emplace_back(std::to_string(tid), 4);
  } else if (p.starts_with("/proc/self/task/")) {
    bool found = false;
    for (auto& [tid, name] : threads::list())
      if (p == "/proc/self/task/" + std::to_string(tid)) found = true;
    if (!found) return false;
    names = {{"status", 8}, {"stat", 8}, {"comm", 8}};
  } else if (p == "/dev")
    names = {{"null", 2}, {"zero", 2}, {"random", 2}, {"urandom", 2}};
  else if (p == "/sys")
    names = {{"devices", 4}};
  else if (p == "/sys/devices")
    names = {{"system", 4}};
  else if (p == "/sys/devices/system")
    names = {{"cpu", 4}};
  else if (p == "/sys/devices/system/cpu") {
    names = {{"possible", 8}, {"present", 8}, {"online", 8}};
    for (unsigned i = 0; i < cpu_count(); ++i) names.emplace_back("cpu" + std::to_string(i), 4);
  } else
    return false;
  if (out) {
    names.insert(names.begin(), {{".", 4}, {"..", 4}});
    for (auto& [name, type] : names) {
      Dirent d{};
      d.ino = out->size() + 1;
      d.off = d.ino;
      d.type = type;
      d.reclen = sizeof(d);
      std::memcpy(d.name, name.data(), std::min<std::size_t>(name.size(), 255));
      out->push_back(d);
    }
  }
  return true;
}
std::string proc_file(std::string_view p, bool* exists) {
  *exists = true;
  std::string alias;
  // Only task paths need names. Most proc polling needs no thread snapshot.
  std::vector<std::pair<std::int32_t, std::string>> live;
  if (p.starts_with("/proc/self/task/")) live = threads::list();
  int visible_pid = kGuestPid;
  std::string visible_name = process_config().package.substr(0, 15);
  for (auto& [tid, name] : live) {
    auto prefix = "/proc/self/task/" + std::to_string(tid) + "/";
    if (p.starts_with(prefix)) {
      auto part = p.substr(prefix.size());
      if (part == "comm") return name + "\n";
      if (part == "status" || part == "stat") {
        visible_pid = tid;
        visible_name = name;
        alias = "/proc/self/" + std::string(part);
        p = alias;
        break;
      }
    }
  }
  if (auto image = process_image::file(p)) return std::move(*image);
  if (p == "/proc/self/cmdline") return process_config().package + std::string(1, '\0');
  if (p == "/proc/self/cgroup") return "0::/\n";
  if (p == "/proc/self/status") {
    auto status = std::format(
        "Name:\t{}\nState:\tR "
        "(running)\nTgid:\t1000\nPid:\t{}\nPPid:\t1\nUid:\t10125\t10125\t10125\t10125\nGid:"
        "\t10125\t10125\t10125\t10125\nThreads:\t{}\n",
        visible_name, visible_pid, threads::count());
    // Linux Documentation/filesystems/proc.rst: VmRSS is current residency,
    // VmHWM is its high-water mark. Like android.os.Debug, include the host
    // process's JVM/JIT/HLE pages; private commit is not resident or swap usage.
    platform::ProcessResidentMemory memory;
    if (platform::process_resident_memory(memory))
      status += std::format("VmRSS: {} kB\nVmHWM: {} kB\n", memory.resident_kib, memory.peak_resident_kib);
    else log::error("proc", "Process residency query failed; status memory fields unavailable");
    return status;
  }
  if (p == "/proc/self/maps") {
    return AddressSpace::get().proc_maps();
  }
  if (p == "/proc/cpuinfo") {
    std::string features;
    auto hw = cpu_backend().info().hwcap;
    const char* names[] = {"fp",       "asimd", "evtstrm", "aes",    "pmull",   "sha1",
                           "sha2",     "crc32", "atomics", "fphp",   "asimdhp", "cpuid",
                           "asimdrdm", "jscvt", "fcma",    "lrcpc",  "dcpop",   "sha3",
                           "sm3",      "sm4",   "asimddp", "sha512", "sve",     "asimdfhm"};
    // arch/arm64/kernel/cpuinfo.c compat feature names, indexed by
    // COMPAT_HWCAP_* from arch/arm64/include/asm/hwcap.h.
    const char* compat_names[] = {"swp", "half", "thumb", "26bit", "fastmult", "fpa", "vfp", "edsp",
      "java", "iwmmxt", "crunch", "thumbee", "neon", "vfpv3", "vfpv3d16", "tls",
      "vfpv4", "idiva", "idivt", "vfpd32", "lpae", "evtstrm"};
    auto feature_names = process_config().isa() == GuestIsa::a32 ? std::span<const char* const>(compat_names) : std::span<const char* const>(names);
    for (unsigned i = 0; i < feature_names.size(); ++i)
      if (hw & (1ull << i)) features += " " + std::string(feature_names[i]);
    std::string text;
    for (unsigned i = 0; i < cpu_count(); ++i)
      text += std::format(
          "processor\t: {}\nBogoMIPS\t: 38.40\nFeatures\t:{}\nCPU implementer\t: 0x41\nCPU "
          "architecture: 8\nCPU variant\t: 0x0\nCPU part\t: 0xd0b\nCPU revision\t: 0\n\n",
          i, features);
    return text;
  }
  if (p == "/proc/meminfo") {
    auto kb = guest_usable_memory() / 1024;
    return std::format("MemTotal: {:14} kB\nMemFree: {:15} kB\nMemAvailable: {:10} kB\n"
      "Buffers:               0 kB\nCached:                0 kB\nSwapTotal:             0 kB\nSwapFree:              0 kB\n", kb, kb / 2, kb * 3 / 4);
  }
  if (p == "/proc/stat") {
    std::string s = "cpu 0 0 0 0 0 0 0 0 0 0\n";
    const auto cpus = cpu_count();
    s.reserve(32 * (cpus + 1));
    for (unsigned i = 0; i < cpus; ++i)
      std::format_to(std::back_inserter(s), "cpu{} 0 0 0 0 0 0 0 0 0 0\n", i);
    return s;
  }
  if (p == "/proc/self/stat") {
    std::string text = std::format("{} ({}) R", visible_pid, visible_name);
    const auto memory = AddressSpace::get().mapped_size();
    platform::ProcessResidentMemory resident;
    const bool have_resident = platform::process_resident_memory(resident);
    const auto thread_count = threads::count();
    text.reserve(256);
    for (int field = 4; field <= 52; ++field) {
      std::int64_t value = 0;
      if (field == 4) value = 1;
      if (field == 5 || field == 6) value = 1000;
      if (field == 8) value = -1;
      if (field == 14) value = host::cpu_time(false) / 10000000;
      if (field == 18) value = 20;
      if (field == 20) value = thread_count;
      if (field == 23) value = memory;
      // stat field 24 is resident pages, not the virtual mapping size.
      if (field == 24 && have_resident) value = resident.resident_kib / 4;
      text += ' ' + std::to_string(value);
    }
    return text + "\n";
  }
  if (p == "/proc/self/auxv" && process_image::published()) return process_image::auxv();
  if (p == "/proc/self/auxv") {
    std::uint64_t aux[] = {6,  4096,
                           16, cpu_backend().info().hwcap,
                           26, cpu_backend().info().hwcap2,
                           11, 10125,
                           12, 10125,
                           13, 10125,
                           14, 10125,
                           0,  0};
    if (process_config().isa() == GuestIsa::a32) {
      std::uint32_t compat[std::size(aux)];
      std::transform(std::begin(aux), std::end(aux), compat, [](auto v) { return static_cast<std::uint32_t>(v); });
      return std::string(reinterpret_cast<const char*>(compat), sizeof(compat));
    }
    return std::string(reinterpret_cast<const char*>(aux), sizeof(aux));
  }
  if (p == "/sys/devices/system/cpu/possible" || p == "/sys/devices/system/cpu/present" ||
      p == "/sys/devices/system/cpu/online")
    return std::format("0-{}\n", cpu_count() - 1);
  for (unsigned i = 0; i < cpu_count(); ++i) {
    auto root = std::format("/sys/devices/system/cpu/cpu{}/", i);
    if (p == root + "online") return "1\n";
    if (p == root + "cpufreq/cpuinfo_max_freq" || p == root + "cpufreq/scaling_max_freq" ||
        p == root + "cpufreq/scaling_cur_freq")
      return "2400000\n";
    if (p == root + "cpufreq/cpuinfo_min_freq" || p == root + "cpufreq/scaling_min_freq")
      return "300000\n";
  }
  *exists = false;
  return {};
}
}  // namespace venture::hle::libc
