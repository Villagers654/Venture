#pragma once
// Implicit OpenXR API layers that would fail the host instance.
//
// The Khronos loader (ApiLayerInterface::LoadApiLayers) loads every enabled
// implicit layer into xrCreateInstance. A layer whose library does not load
// (LoadLibrary/dlopen fails: not a library, wrong architecture, unreadable,
// a missing dependency, or a DllMain that refuses the process) fails
// xrCreateInstance as a whole with XR_ERROR_FILE_ACCESS_ERROR and "Failed
// loading layer information" when no other layer loads; one that lacks the
// negotiation entry point (XR_ERROR_API_LAYER_NOT_PRESENT) or fails
// negotiation does when no layer loads after it. A layer can also load and
// then fail the instance from its xrCreateApiLayerInstance. Working layers
// stay loaded: only the broken ones are left out, through each manifest's
// disable_environment set as a loader property (XR_EXT_loader_init_properties),
// so the process environment stays as it is.
#include <string>
#include <vector>

namespace venture::xr {
struct ApiLayer {
  std::string name, manifest, disable_environment;
  std::string reason;  // why the layer is broken; empty when it loads and negotiates
};
// The enabled implicit layers the loader would load, in its order, each
// loaded and negotiated with as the loader does to find out whether it works.
std::vector<ApiLayer> enabled_openxr_layers();
// Disables exactly `layers` for this process's loader (xrInitializeLoaderKHR
// with loader properties; an empty list enables them all again); call while
// no instance exists. False when the loader refused.
bool skip_openxr_layers(const std::vector<ApiLayer> &layers);
// While one lives, a library that fails to load on this thread fails quietly:
// Windows otherwise shows a modal "Bad Image" error for a damaged DLL and
// blocks the loader (and the game start) until someone closes it.
class QuietLibraryErrors {
 public:
  QuietLibraryErrors();
  ~QuietLibraryErrors();
  QuietLibraryErrors(const QuietLibraryErrors &) = delete;
  QuietLibraryErrors &operator=(const QuietLibraryErrors &) = delete;

 private:
  [[maybe_unused]] unsigned previous_ = 0;
};
// Platform parts: open a layer library as the loader does (an error text on
// failure), find a symbol in it, close it.
void *open_layer_library(const std::string &path, std::string &error);
void *layer_library_symbol(void *library, const char *name);
void close_layer_library(void *library);
}  // namespace venture::xr
