#pragma once
#include "common/environment.h"
#include "xr/host_xr.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>

namespace venture::test {
struct ScopedMenuMode {
  bool present = std::getenv("VENTURE_MENU_CHORD") != nullptr;
  std::string previous = present ? std::getenv("VENTURE_MENU_CHORD") : "";
  explicit ScopedMenuMode(const char *value) { set(value); }
  ~ScopedMenuMode() { set(present ? previous.c_str() : nullptr); }
  static void set(const char *value) {
#ifdef _WIN32
    _putenv_s("VENTURE_MENU_CHORD", value ? value : "");
#else
    if (value) setenv("VENTURE_MENU_CHORD", value, 1); else unsetenv("VENTURE_MENU_CHORD");
#endif
    venture::environment::refresh();
    venture::xr::invalidate_host_procedures();
  }
};

// A host can advertise Touch while leaving its menu unbound. Override just
// that native action; the real mock still supplies all other input and focus.
struct NativeMenuBindingOverride {
  inline static NativeMenuBindingOverride *current{};
  void *previous = venture::xr::host_xr().get_instance_proc_addr;
  PFN_xrGetInstanceProcAddr fallback = reinterpret_cast<PFN_xrGetInstanceProcAddr>(previous);
  PFN_xrEnumerateBoundSourcesForAction native_bound{};
  XrAction action;
  bool missing = true;
  XrPath appeared_source{};
  XrResult enumeration_result = XR_SUCCESS;
  unsigned bound_queries = 0;
  explicit NativeMenuBindingOverride(XrAction value) : action(value) {
    assert(!current);
    fallback(venture::xr::host_xr().instance, "xrEnumerateBoundSourcesForAction",
             reinterpret_cast<PFN_xrVoidFunction *>(&native_bound));
    assert(native_bound);
    current = this;
    venture::xr::host_xr().get_instance_proc_addr = reinterpret_cast<void *>(get);
    venture::xr::invalidate_host_procedures();
  }
  ~NativeMenuBindingOverride() {
    venture::xr::host_xr().get_instance_proc_addr = previous;
    current = nullptr;
    venture::xr::invalidate_host_procedures();
  }
  static XrResult XRAPI_CALL bound(XrSession session,
      const XrBoundSourcesForActionEnumerateInfo *info, std::uint32_t capacity,
      std::uint32_t *count, XrPath *sources) {
    auto &self = *current;
    if (info->action != self.action)
      return self.native_bound(session, info, capacity, count, sources);
    ++self.bound_queries;
    if (XR_FAILED(self.enumeration_result)) return self.enumeration_result;
    if (self.missing) { *count = 0; return XR_SUCCESS; }
    if (self.appeared_source) {
      *count = 1;
      if (capacity) sources[0] = self.appeared_source;
      return XR_SUCCESS;
    }
    return self.native_bound(session, info, capacity, count, sources);
  }
  static XrResult XRAPI_CALL get(XrInstance instance, const char *name, PFN_xrVoidFunction *out) {
    if (std::strcmp(name, "xrEnumerateBoundSourcesForAction") == 0)
      *out = reinterpret_cast<PFN_xrVoidFunction>(bound);
    else return current->fallback(instance, name, out);
    return XR_SUCCESS;
  }
};
} // namespace venture::test
