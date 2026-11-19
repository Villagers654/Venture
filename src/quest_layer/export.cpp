// The layer library's exports: the loader's negotiation entry point, and the
// report the headset check and the tests read.
#include "quest_layer/layer.h"

#include <algorithm>
#include <cstring>
#include <string>

#if defined(_WIN32)
#define VENTURE_LAYER_EXPORT __declspec(dllexport)
#else
#define VENTURE_LAYER_EXPORT __attribute__((visibility("default")))
#endif

extern "C" VENTURE_LAYER_EXPORT XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loader, const char* layer_name, XrNegotiateApiLayerRequest* request) {
  return venture::quest_layer::negotiate(loader, layer_name, request);
}

// Writes the newest instance's report as "lowered=0|1;emulated=a,b;refresh_fallbacks=N"
// into buffer (NUL-terminated, truncated to size). Returns the full length.
extern "C" VENTURE_LAYER_EXPORT std::size_t ventureQuestLayerReport(char* buffer, std::size_t size) {
  const auto report = venture::quest_layer::report();
  const auto text = "lowered=" + std::string(report.lowered ? "1" : "0") + ";emulated=" + report.emulated +
                    ";refresh_fallbacks=" + std::to_string(report.refresh_fallbacks);
  if (buffer && size) {
    const auto n = std::min(size - 1, text.size());
    std::memcpy(buffer, text.data(), n);
    buffer[n] = 0;
  }
  return text.size();
}
