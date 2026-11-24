#pragma once
#include "xr/api.h"
#include <cstdint>
#include <deque>

namespace venture::xr {
// Brief focus dips a game never sees. Some runtimes take focus away for a few
// milliseconds to two seconds while the headset stays worn: a wear sensor that
// flickers "off" for 0.5-2 s, a system overlay that appears and goes. The Steam
// Frame's SteamVR moved H3VR2 FOCUSED -> VISIBLE -> SYNCHRONIZED and back six
// times in three minutes (5 to 523 ms). A Quest game pauses, recentres or drops
// its view on each one, which a Quest never makes it do.
//
// Once the game has been focused for kMinFocusedNs, a host VISIBLE for the
// game's session is held back with what follows it. If FOCUSED returns within
// kMaxDipNs the held changes are dropped (and FOCUSED with them); otherwise, or
// when the session moves on to STOPPING, LOSS_PENDING, EXITING or any other
// state, they are delivered late and in order, so taking the headset off, a
// long menu visit and quitting still reach the game. Other events pass at once.
class FocusDips {
 public:
  static constexpr std::int64_t kMinFocusedNs = 1'000'000'000;
  static constexpr std::int64_t kMaxDipNs = 2'500'000'000;
  struct Hidden { std::int64_t ns = 0; std::size_t changes = 0; };

  // The next event for the game. host(XrEventDataBuffer&) polls the runtime.
  // `hidden` is set when a dip was just dropped; `late` when held changes are
  // being delivered after kMaxDipNs.
  template <class Host>
  XrResult poll(XrSession session, std::int64_t now, XrEventDataBuffer& out, Host&& host, Hidden* hidden = nullptr,
                bool* late = nullptr) {
    for (;;) {
      if (!release_.empty()) {
        out = release_.front();
        release_.pop_front();
        note(session, out, now);
        return XR_SUCCESS;
      }
      out = {XR_TYPE_EVENT_DATA_BUFFER};
      const XrResult result = host(out);
      if (result != XR_SUCCESS) {
        if (!held_.empty() && now - held_since_ > kMaxDipNs) {
          if (late) *late = true;
          release_held();
          continue;
        }
        return result;
      }
      const int state = state_of(session, out);
      if (state < 0) return result;  // other events and other sessions pass at once
      if (!held_.empty()) {
        if (state == XR_SESSION_STATE_FOCUSED) {
          if (hidden) *hidden = {now - held_since_, held_.size() + 1};
          held_.clear();
          continue;  // the game never left FOCUSED
        }
        held_.push_back(out);
        if (state != XR_SESSION_STATE_VISIBLE && state != XR_SESSION_STATE_SYNCHRONIZED) release_held();
        continue;
      }
      if (state == XR_SESSION_STATE_VISIBLE && focused_ && now - focused_since_ >= kMinFocusedNs) {
        held_since_ = now;
        held_.push_back(out);
        continue;
      }
      note(session, out, now);
      return result;
    }
  }
  bool holding() const { return !held_.empty(); }
  // A new session starts unfocused with nothing held.
  void reset() { *this = {}; }

 private:
  static int state_of(XrSession session, const XrEventDataBuffer& e) {
    if (e.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) return -1;
    const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(e);
    return changed.session == session ? int(changed.state) : -1;
  }
  void note(XrSession session, const XrEventDataBuffer& e, std::int64_t now) {
    const int state = state_of(session, e);
    if (state < 0) return;
    if (state == XR_SESSION_STATE_FOCUSED && !focused_) focused_since_ = now;
    focused_ = state == XR_SESSION_STATE_FOCUSED;
  }
  void release_held() {
    release_.insert(release_.end(), held_.begin(), held_.end());
    held_.clear();
  }
  std::deque<XrEventDataBuffer> held_, release_;
  std::int64_t held_since_ = 0, focused_since_ = 0;
  bool focused_ = false;
};
}  // namespace venture::xr
