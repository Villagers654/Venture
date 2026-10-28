#include "audio_test_support.h"
#include "audio/audio.h"
#include "core/linker.h"
#include "core/hle.h"
#include "core/thread.h"
#include <array>
#include <algorithm>
#include <deque>
#include <mutex>
namespace venture::test::audio_core {
GuestAddr add_callback(Callback fn) {
  static std::mutex mutex;
  static std::deque<Callback> callbacks;
  std::lock_guard lock(mutex);
  auto* saved = &callbacks.emplace_back(std::move(fn));
  return hle::make_thunk("audio-test", "callback", [](hle::CallContext& ctx) {
    std::array<std::uint64_t, 8> args;
    for (int i = 0; i < 8; ++i) args[i] = ctx.x(i);
    ctx.return_x((*static_cast<Callback*>(ctx.user()))(args));
  }, saved);
}
std::int64_t call(GuestAddr thunk, std::initializer_list<std::uint64_t> args) {
  return static_cast<std::int64_t>(GuestThread::adopt_current().call(thunk, GuestCallArgs{std::span(args.begin(), args.size()), {}}).x0);
}
GuestAddr symbol(std::string_view library, std::string_view name) {
  auto* lib = Linker::get().open(library, kRtldNow, nullptr);
  return lib ? lib->find_symbol(name) : 0;
}
}
namespace venture::audio {
struct BackendTestAccess {
  static bool stop_null_output() {
    auto& b = Backend::get();
    std::lock_guard lock(b.device_lock_);
    return b.null_ && b.playback_ready_ &&
           ma_device_stop(&b.playback_) == MA_SUCCESS;
  }
};
}  // namespace venture::audio
namespace venture::test::audio_core {
bool stop_null_output() { return audio::BackendTestAccess::stop_null_output(); }
}
