// Android tests execute HLE thunks and callbacks through the real runtime core.
#include "android/errors.h"
#include "android_test_support.h"
#include "android/libc_bridge.h"
#include "core/hle.h"
#include "hle/registry.h"
#include "hle/libc/vfs.h"
#include "core/linker.h"
#include "hle/libandroid/libandroid.h"
#include <array>
#include <filesystem>
#include <map>
#include <mutex>
#include <stdexcept>
namespace venture {
namespace test {
void init() {
  static std::once_flag once;
  std::call_once(once, [] {
    register_all_hle_libraries();
    hle::register_libandroid();
    hle::register_libmediandk();
    hle::register_libcamera2ndk();
  });
}
GuestCallResult android_call(const char* lib, const char* name, std::initializer_list<std::uint64_t> args,
                             std::initializer_list<double> fp) {
  init();
  auto* library = Linker::get().open(lib, kRtldNow, nullptr);
  auto thunk = library ? library->find_symbol(name) : 0;
  if (!thunk) throw std::runtime_error(std::string("Missing HLE symbol ") + name);
  return GuestThread::adopt_current().call(thunk, GuestCallArgs{std::span(args.begin(), args.size()), std::span(fp.begin(), fp.size())});
}
namespace {
using Callback = std::function<GuestCallResult(const GuestCallArgs&)>;
std::mutex callback_mutex;
std::map<GuestAddr, std::shared_ptr<Callback>> callbacks;
}
GuestAddr android_callback(Callback fn) {
  auto address = hle::make_thunk("android-test", "callback", [](hle::CallContext& ctx) {
    std::shared_ptr<Callback> callback;
    {
      std::lock_guard lock(callback_mutex);
      callback = callbacks.at(ctx.info().thunk);
    }
    std::array<std::uint64_t, 8> args;
    std::array<double, 8> fp;
    for (int i = 0; i < 8; ++i) { args[i] = ctx.x(i); fp[i] = ctx.d(i); }
    auto result = (*callback)(GuestCallArgs{args, fp});
    ctx.return_x(result.x0);
    ctx.return_x1(result.x1);
    ctx.cpu().set_v(0, result.v0);
  });
  std::lock_guard lock(callback_mutex);
  callbacks.emplace(address, std::make_shared<Callback>(std::move(fn)));
  return address;
}
void android_remove_callback(GuestAddr address) {
  std::lock_guard lock(callback_mutex);
  callbacks.erase(address);
}
bool android_has_symbol(const char* lib, const std::string& name) {
  init();
  auto* library = Linker::get().open(lib, kRtldNow, nullptr);
  return library && library->find_symbol(name) != 0;
}
// Explicit readiness injection isolates looper scheduling; descriptors live in the real VFS.
class ReadyFile final : public hle::libc::File {
 public:
  std::atomic<short> events{0};
  std::atomic<unsigned> scans{0};
  bool readiness_notifications() const override { return true; }
  short ready(short requested) override { ++scans; return events.load() & (requested | 8 | 16 | 32); }
};
int android_test_fd() { return hle::libc::FdTable::get().insert(std::make_shared<ReadyFile>()); }
void android_fd_ready(int fd, std::int16_t events) {
  auto file = std::dynamic_pointer_cast<ReadyFile>(hle::libc::FdTable::get().file(fd));
  if (!file) throw std::runtime_error("not a test readiness descriptor");
  file->events = events;
  file->readiness.notify(events);
}
unsigned android_fd_scans(int fd) {
  auto file = std::dynamic_pointer_cast<ReadyFile>(hle::libc::FdTable::get().file(fd));
  return file ? file->scans.load() : 0;
}
bool android_fd_subscribed(int fd) {
  auto file = std::dynamic_pointer_cast<ReadyFile>(hle::libc::FdTable::get().file(fd));
  return file && file->readiness.interests();
}
void android_close_fd(int fd) { hle::libc::FdTable::get().close(fd); }
std::string android_fd_path(int fd) {
  auto file = hle::libc::FdTable::get().file(fd);
  if (!file) throw std::runtime_error("invalid test descriptor");
  return file->path;
}
}
}
