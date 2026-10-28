#pragma once
#include "core/elf.h"
#include <mutex>
namespace venture {
// One recursive lock guards loader metadata, constructor/finalizer ordering and
// publication. Keep it across guest callbacks: reentry is allowed on the owner
// thread, while another thread cannot observe or retire a half-initialized DSO.
class LoaderMutex {
 public:
  void lock() { mutex_.lock(); } // Internal metadata and lifecycle scopes never cancel.
  void lock_guest();
  bool lock_cleanup() noexcept;
  bool try_lock() { return mutex_.try_lock(); }
  void unlock() { mutex_.unlock(); }
 private:
  std::recursive_timed_mutex mutex_;
};
LoaderMutex& linker_lifecycle_mutex();
std::unique_lock<LoaderMutex> guest_loader_lock();
// Inspection suppresses guest init/ifunc execution and records deferred resolvers explicitly.
void linker_set_inspection(bool enabled);
std::vector<elf::Report> linker_reports();
void linker_finalize();
// The ABI packet installs this before inspection; called once per A32 import,
// including guest-resolved, weak and unreferenced symbols. Returned text goes in JSON.
using ImportCoverageHook = std::function<std::string(const elf::Image&, const elf::Import&)>;
void linker_set_import_coverage_hook(ImportCoverageHook hook);
}
