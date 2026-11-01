#include "common/environment.h"
#include "platform/runtime_stop.h"
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <string>
#include <stdexcept>
namespace venture::platform {
namespace {
std::atomic<bool> stopped{false};
std::atomic<DWORD> stop_event{0};
BOOL WINAPI control(DWORD event) {
  if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT ||
      event == CTRL_LOGOFF_EVENT || event == CTRL_SHUTDOWN_EVENT) {
    stop_event.store(event);
    stopped.store(true);
    return TRUE;
  }
  return FALSE;
}
}
struct RuntimeStop::Impl {
  HANDLE parent = nullptr;
  HANDLE event = nullptr;
  HANDLE local_event = nullptr;
  bool parent_gone = false;
  ~Impl() { if (parent) CloseHandle(parent); if (event) CloseHandle(event); if (local_event) CloseHandle(local_event); }
};
RuntimeStop::RuntimeStop(std::uint32_t pid) : impl_(std::make_unique<Impl>()) {
  const auto local_name = "Local\\Venture.Stop." + std::to_string(GetCurrentProcessId());
  impl_->local_event = CreateEventA(nullptr, TRUE, FALSE, local_name.c_str());
  if (!impl_->local_event) throw std::runtime_error("Cannot create runtime shutdown event");
  // gpu_policy owns this manual-reset event. Its unique name is inherited by
  // the supervisor and app workers; setting it requests their normal teardown.
  const auto* name = venture::environment::getenv("VENTURE_STOP_EVENT");
  if (name && *name) {
    impl_->event = OpenEventA(SYNCHRONIZE, FALSE, name);
    if (!impl_->event) throw std::runtime_error("Cannot open runtime shutdown event");
  }
  if (pid) {
    impl_->parent = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!impl_->parent) {
      if (GetLastError() != ERROR_INVALID_PARAMETER)
        throw std::runtime_error("Cannot monitor launcher process");
      impl_->parent_gone = true;
    }
  }
  stopped.store(false);
  if (!SetConsoleCtrlHandler(control, TRUE)) throw std::runtime_error("Cannot install runtime shutdown handler");
}
RuntimeStop::~RuntimeStop() { SetConsoleCtrlHandler(control, FALSE); }
bool RuntimeStop::requested() const {
  return stopped.load() || impl_->parent_gone ||
         (impl_->event && WaitForSingleObject(impl_->event, 0) == WAIT_OBJECT_0) ||
         WaitForSingleObject(impl_->local_event, 0) == WAIT_OBJECT_0 ||
         (impl_->parent && WaitForSingleObject(impl_->parent, 0) == WAIT_OBJECT_0);
}
const char* RuntimeStop::reason() const {
  if (stopped.load()) {
    switch (stop_event.load()) {
      case CTRL_C_EVENT: return "console Ctrl+C";
      case CTRL_BREAK_EVENT: return "console Ctrl+Break";
      case CTRL_CLOSE_EVENT: return "console window closed";
      case CTRL_LOGOFF_EVENT: return "user logoff";
      default: return "system shutdown";
    }
  }
  if (impl_->parent_gone) return "launcher process was already gone at start";
  if (impl_->event && WaitForSingleObject(impl_->event, 0) == WAIT_OBJECT_0) return "supervisor stop event";
  if (WaitForSingleObject(impl_->local_event, 0) == WAIT_OBJECT_0) return "launcher stop request";
  if (impl_->parent && WaitForSingleObject(impl_->parent, 0) == WAIT_OBJECT_0) return "launcher process exited";
  return "none";
}
bool request_runtime_stop(std::uint32_t pid) {
  const auto name = "Local\\Venture.Stop." + std::to_string(pid);
  auto event = OpenEventA(EVENT_MODIFY_STATE, FALSE, name.c_str());
  if (!event) return false;
  const bool result = SetEvent(event);
  CloseHandle(event);
  return result;
}
}
