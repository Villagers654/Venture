#include "xr/openxr_layers.h"
#include "common/openxr_layers.h"
#include "common/paths.h"
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

namespace venture::xr {
namespace {
// What LoadApiLayers does with one layer, short of creating an instance.
std::string load_problem(const openxr_layers::Manifest &layer) {
  std::string error;
  void *library = open_layer_library(paths::utf8(layer.library), error);
  if (!library) return "its library " + paths::utf8(layer.library) + " does not load (" + error + ")";
  std::string entry = "xrNegotiateLoaderApiLayerInterface";
  if (const auto renamed = layer.functions.find(entry); renamed != layer.functions.end()) entry = renamed->second;
  std::string problem;
  if (auto negotiate =
          reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(layer_library_symbol(library, entry.c_str()));
      !negotiate) {
    problem = "its library " + paths::utf8(layer.library) + " has no " + entry;
  } else {
    XrNegotiateLoaderInfo loader{};
    loader.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    loader.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    loader.structSize = sizeof(loader);
    loader.minInterfaceVersion = 1;
    loader.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    loader.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    loader.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
    XrNegotiateApiLayerRequest request{};
    request.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST;
    request.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION;
    request.structSize = sizeof(request);
    const XrResult result = negotiate(&loader, layer.name.c_str(), &request);
    if (XR_FAILED(result)) problem = "it refused the loader's interface (XrResult " + std::to_string(result) + ")";
    else if (!request.getInstanceProcAddr) problem = "it returned no xrGetInstanceProcAddr";
  }
  close_layer_library(library);
  return problem;
}
}  // namespace

std::vector<ApiLayer> enabled_openxr_layers() {
  std::vector<ApiLayer> out;
  for (const auto &path : openxr_layers::implicit_manifests()) {
    const auto layer = openxr_layers::read(path);
    // The loader passes over these itself: an unusable manifest, a disabled
    // layer, a library path that names no file.
    if (layer.name.empty() || layer.problem.starts_with("the manifest") || !openxr_layers::enabled(layer)) continue;
    std::error_code ec;
    if (layer.library.has_parent_path() && !std::filesystem::exists(layer.library, ec)) continue;
    out.push_back({layer.name, paths::utf8(path), layer.disable_environment, load_problem(layer)});
  }
  return out;
}

bool skip_openxr_layers(const std::vector<ApiLayer> &layers) {
  PFN_xrVoidFunction raw = nullptr;
  if (XR_FAILED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", &raw)) || !raw) return false;
  std::vector<XrLoaderInitPropertyValueEXT> values;
  for (const auto &layer : layers) values.push_back({layer.disable_environment.c_str(), "1"});
  XrLoaderInitInfoPropertiesEXT info{XR_TYPE_LOADER_INIT_INFO_PROPERTIES_EXT};
  info.propertyValueCount = static_cast<std::uint32_t>(values.size());
  info.propertyValues = values.data();
  return XR_SUCCEEDED(reinterpret_cast<PFN_xrInitializeLoaderKHR>(raw)(
      reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&info)));
}
}  // namespace venture::xr
