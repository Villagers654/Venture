#include "hle/steam/library.h"
#include "hle/steam/bridge.h"
#include "core/linker.h"
#include "core/process.h"
#include "core/thread.h"
#include "core/thread_runtime.h"
#include "core/error.h"
#include "platform/steam_client.h"
#include "common/log.h"
#include "common/paths.h"
#include <miniz.h>
#include <cstring>

namespace venture::steam {
namespace {
struct IdentityMismatch : std::runtime_error { using std::runtime_error::runtime_error; };
int init(const char* versions, char* error) { return Bridge::get().initialize(versions,error); }
int init_flat(char* error) { return init(nullptr,error); }
bool init_bool() { return init(nullptr,nullptr) == 0; }
void shutdown() { Bridge::get().shutdown(); }
void release_memory() { Bridge::get().release_thread(false); }
bool restart(std::uint32_t appid) {
  if (appid != Bridge::get().appid()) {
    throw IdentityMismatch("RestartAppIfNecessary app ID differs from plan; refused");
  }
  return false; // Local by design: forwarding would start the Windows game.
}
std::int32_t pipe() { auto& b=Bridge::get(); std::lock_guard lock(b.mutex); return b.pipe(); }
std::int32_t user() { auto& b=Bridge::get(); std::lock_guard lock(b.mutex); return b.user(); }
void manual_init() {
  auto& b=Bridge::get(); std::lock_guard lock(b.mutex);
  if (!b.ready()) throw std::runtime_error("Steam manual dispatch before initialization");
  // H2 is always in manual-dispatch mode; no standard callback registry exists.
}
void frame(std::int32_t pipe) { Bridge::get().run_frame(pipe); }
bool next(std::int32_t pipe, void* msg) { return Bridge::get().next_callback(pipe,msg); }
void free_last(std::int32_t pipe) { Bridge::get().free_callback(pipe); }
bool result(std::int32_t pipe, std::uint64_t call, void* data, std::int32_t size, std::int32_t id, bool* failed) {
  return Bridge::get().call_result(pipe,call,data,size,id,failed);
}
bool utils_result(GuestAddr self,std::uint64_t call,void* data,std::int32_t size,std::int32_t id,bool* failed) {
  Call c(self,"ISteamUtils");
  return c.bridge.call_result(c.bridge.pipe(),call,data,size,id,failed);
}
GuestAddr client() { return Bridge::get().interface("SteamClient023"); }
GuestAddr create_interface(const char* version) {
  if (!version) return 0;
  // Returned handles are only for the flat API, never advertised as guest
  // Itanium-vtable objects. ContextInit/C++ callback APIs remain unregistered.
  return Bridge::get().interface(version);
}
GuestAddr find_user(std::int32_t requested_user,const char* version) {
  auto& b=Bridge::get(); std::lock_guard lock(b.mutex);
  if (!requested_user || requested_user != b.user()) return 0;
  return create_interface(version);
}
bool running() {
  // This query is valid before Init and does not mean that the user is online
  // or owns the game. Those checks belong to initialize().
  return platform::steam_client_running();
}
void guarded(hle::CallContext& ctx) {
  auto handler = reinterpret_cast<hle::Handler>(ctx.user());
  try { handler(ctx); }
  catch (const std::exception& e) {
    ctx.thread().set_errno(dynamic_cast<const IdentityMismatch*>(&e) ? err::kEPERM : err::kEIO);
    log::error("steam", "{}: {}",ctx.info().name,e.what());
    throw; // Fail observably; never fabricate a successful service result.
  }
}
void add(HleLibrary& lib,std::string_view name,hle::Handler handler) {
  lib.add_function(name,hle::make_thunk("libsteam_api.so",name,guarded,reinterpret_cast<void*>(handler)));
}
}
void add_exports(HleLibrary& lib,const Sdk& sdk) {
  for (const auto& e:sdk.exports) add(lib,e.name,e.handler);
#define EXPORT(name, fn) add(lib,name,hle::wrap<&fn>())
  EXPORT("SteamInternal_SteamAPI_Init",init);
  EXPORT("SteamAPI_InitFlat",init_flat);
  EXPORT("SteamAPI_Init",init_bool);
  EXPORT("SteamAPI_InitSafe",init_bool);
  EXPORT("SteamAPI_Shutdown",shutdown);
  EXPORT("SteamAPI_RestartAppIfNecessary",restart);
  EXPORT("SteamAPI_IsSteamRunning",running);
  EXPORT("SteamAPI_GetHSteamPipe",pipe);
  EXPORT("SteamAPI_GetHSteamUser",user);
  EXPORT("SteamAPI_ManualDispatch_Init",manual_init);
  EXPORT("SteamAPI_ManualDispatch_RunFrame",frame);
  EXPORT("SteamAPI_ManualDispatch_GetNextCallback",next);
  EXPORT("SteamAPI_ManualDispatch_FreeLastCallback",free_last);
  EXPORT("SteamAPI_ManualDispatch_GetAPICallResult",result);
  EXPORT("SteamAPI_ISteamUtils_GetAPICallResult",utils_result);
  EXPORT("SteamAPI_ReleaseCurrentThreadMemory",release_memory);
  EXPORT("SteamClient",client);
  EXPORT("SteamAPI_SteamClient_v023",client);
  EXPORT("SteamInternal_CreateInterface",create_interface);
  EXPORT("SteamInternal_FindOrCreateUserInterface",find_user);
#undef EXPORT
}
void register_library(const Plan& plan,const fs::path& apk) {
  if (process_config().isa()!=GuestIsa::a64) throw std::runtime_error("Steam bridge requires ARM64 guest");
  mz_zip_archive zip{};
  if (!mz_zip_reader_init_file(&zip,paths::utf8(apk).c_str(),0)) throw std::runtime_error("Cannot open Steam APK");
  struct End { mz_zip_archive* zip; ~End(){ mz_zip_reader_end(zip); } } end{&zip};
  const auto index=mz_zip_reader_locate_file(&zip,"lib/arm64-v8a/libsteam_api.so",nullptr,0);
  mz_zip_archive_file_stat stat{};
  if(index<0 || !mz_zip_reader_file_stat(&zip,index,&stat) || stat.m_uncomp_size>16*1024*1024)
    throw std::runtime_error("Steam APK has no bounded libsteam_api.so");
  std::vector<std::byte> bytes(stat.m_uncomp_size);
  if(!mz_zip_reader_extract_to_mem(&zip,index,bytes.data(),bytes.size(),0)) throw std::runtime_error("Cannot read Steam SDK from APK");
  const auto* sdk=detect_sdk(bytes);
  if(!sdk) throw std::runtime_error("Unknown Android Steamworks SDK hash; supported pins: 1.63, 1.64, 1.65a");
  log::info("steam","Verified guest Steamworks SDK {}",sdk->version);
  Bridge::get().configure(plan,*sdk);
  auto lib=std::make_shared<HleLibrary>("libsteam_api.so",std::vector<std::string>{
      "steam_api","libsteam_api","!/lib/arm64-v8a/libsteam_api.so"});
  add_exports(*lib,*sdk);
  Linker::get().register_hle_library(std::move(lib));
  set_guest_thread_native_cleanup(release_steam_thread);
}
} // namespace venture::steam
