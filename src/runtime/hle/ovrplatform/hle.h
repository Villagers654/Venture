#pragma once
#include "core/hle.h"
#include "hle/ovrplatform/session.h"

namespace venture {
class HleLibrary;
class PipeChannel;
}
namespace venture::ovrplatform {
struct Export {
  const char* name;
  hle::Handler handler;
};
std::span<const Export> exports();
// Integration entry points: call before any guest ELF is loaded. Keep the
// session alive until guest threads stop (the thunks retain a Session pointer).
std::unique_ptr<Session> make_runtime_session(const AppIdentity&);
std::unique_ptr<Session> make_runtime_session(const AppIdentity&,
                                              std::shared_ptr<Provider>);
std::unique_ptr<Session> make_runtime_session(
    const AppIdentity&, const std::filesystem::path&,
    std::shared_ptr<PipeChannel>);
std::shared_ptr<HleLibrary> make_library(Session&);
void register_library(Session&);
}  // namespace venture::ovrplatform
