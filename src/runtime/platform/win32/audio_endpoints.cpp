#include "platform/audio_backend.h"
#include "platform/audio_endpoints.h"
#include "platform/win32/audio_endpoints.h"

namespace venture::platform {
namespace {
// MMDevice API endpoint notifications (IMMNotificationClient). The client and
// enumerator live for the process: callbacks may arrive until exit.
class EndpointClient final : public IMMNotificationClient {
 public:
  EndpointClient(void (*changed)(void*), void* context) : changed_(changed), context_(context) {}
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
    if (id == __uuidof(IUnknown) || id == __uuidof(IMMNotificationClient)) {
      *out = static_cast<IMMNotificationClient*>(this);
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return notify(); }
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return notify(); }
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return notify(); }
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return notify(); }
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return notify(); }

 private:
  HRESULT notify() {
    changed_(context_);
    return S_OK;
  }
  void (*changed_)(void*);
  void* context_;
};
} // namespace

bool watch_audio_endpoints(void (*changed)(void*), void* context, RegisterEndpointClient register_client) {
  auto* client = new EndpointClient(changed, context);
  if (register_client(client)) return true;
  delete client;
  return false;
}

bool watch_audio_endpoints(void (*changed)(void*), void* context) {
  return watch_audio_endpoints(changed, context, [](IMMNotificationClient* client) {
    // Keep the process MTA alive without dedicating a thread to it.
    CO_MTA_USAGE_COOKIE cookie{};
    if (FAILED(CoIncrementMTAUsage(&cookie))) return false;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) return false; // an STA caller keeps per-call enumeration
    IMMDeviceEnumerator* enumerator = nullptr;
    bool watched = SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                              __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator)));
    if (watched) {
      watched = SUCCEEDED(enumerator->RegisterEndpointNotificationCallback(client));
      if (!watched) enumerator->Release();
    }
    CoUninitialize();
    return watched;
  });
}
// miniaudio's built-in WASAPI backend serves Windows.
void configure_audio_context(ma_context_config&) {}
} // namespace venture::platform
