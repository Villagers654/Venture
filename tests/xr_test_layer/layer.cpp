// A pass-through implicit OpenXR API layer for the host instance tests: it
// forwards every call and counts the instances created through it. A
// manifest that renames the negotiation entry point to
// ventureTestFailingNegotiate gets a layer that loads and then fails every
// xrCreateInstance.
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <cstring>

#if defined(_WIN32)
#define VENTURE_LAYER_EXPORT __declspec(dllexport)
#else
#define VENTURE_LAYER_EXPORT __attribute__((visibility("default")))
#endif

namespace {
PFN_xrGetInstanceProcAddr next_get_instance_proc_addr = nullptr;
int instances = 0;

XRAPI_ATTR XrResult XRAPI_CALL get_instance_proc_addr(XrInstance instance, const char *name,
                                                     PFN_xrVoidFunction *function) {
  if (!next_get_instance_proc_addr) return XR_ERROR_FUNCTION_UNSUPPORTED;
  return next_get_instance_proc_addr(instance, name, function);
}

XRAPI_ATTR XrResult XRAPI_CALL create_api_layer_instance(const XrInstanceCreateInfo *info,
                                                        const XrApiLayerCreateInfo *layer_info,
                                                        XrInstance *instance) {
  if (!layer_info || !layer_info->nextInfo) return XR_ERROR_INITIALIZATION_FAILED;
  next_get_instance_proc_addr = layer_info->nextInfo->nextGetInstanceProcAddr;
  XrApiLayerCreateInfo next = *layer_info;
  next.nextInfo = layer_info->nextInfo->next;
  const XrResult result = layer_info->nextInfo->nextCreateApiLayerInstance(info, &next, instance);
  if (XR_SUCCEEDED(result)) ++instances;
  return result;
}
// The failing layer never has an instance to dispatch for.
XRAPI_ATTR XrResult XRAPI_CALL failing_get_instance_proc_addr(XrInstance, const char *, PFN_xrVoidFunction *function) {
  if (function) *function = nullptr;
  return XR_ERROR_FUNCTION_UNSUPPORTED;
}
XRAPI_ATTR XrResult XRAPI_CALL failing_create_api_layer_instance(const XrInstanceCreateInfo *,
                                                                const XrApiLayerCreateInfo *, XrInstance *) {
  return XR_ERROR_RUNTIME_FAILURE;
}

XrResult negotiate(const XrNegotiateLoaderInfo *loader, XrNegotiateApiLayerRequest *request,
                   PFN_xrGetInstanceProcAddr get, PFN_xrCreateApiLayerInstance create) {
  if (!loader || !request || loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
      request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
      loader->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION ||
      loader->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION)
    return XR_ERROR_INITIALIZATION_FAILED;
  request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
  request->layerApiVersion = XR_MAKE_VERSION(1, 0, 0);
  request->getInstanceProcAddr = get;
  request->createApiLayerInstance = create;
  return XR_SUCCESS;
}
}  // namespace

extern "C" VENTURE_LAYER_EXPORT XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo *loader, const char *, XrNegotiateApiLayerRequest *request) {
  return negotiate(loader, request, get_instance_proc_addr, create_api_layer_instance);
}

extern "C" VENTURE_LAYER_EXPORT XrResult XRAPI_CALL ventureTestFailingNegotiate(
    const XrNegotiateLoaderInfo *loader, const char *, XrNegotiateApiLayerRequest *request) {
  return negotiate(loader, request, failing_get_instance_proc_addr, failing_create_api_layer_instance);
}

// Instances created through this layer, for the tests.
extern "C" VENTURE_LAYER_EXPORT int ventureTestLayerInstances() { return instances; }
