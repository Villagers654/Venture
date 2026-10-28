#pragma once
#include "common/steam.h"
#include "hle/steam/abi.h"
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace venture::steam {
// MSVC x64 virtual calls are explicit C function-pointer calls. Indices come
// from the pinned clang MSVC dump, never the MinGW class declaration order.
template<class R, class... A> R member(void* self, std::uint16_t slot, A... args) {
  using Fn = R (*)(void*, A...);
  auto function = reinterpret_cast<Fn>((*static_cast<void***>(self))[slot]);
  return function(self, args...);
}
struct HostCallback { std::int32_t user, id; void* data; std::int32_t bytes; };
static_assert(sizeof(HostCallback) == 24 && offsetof(HostCallback, data) == 8);
class Bridge {
  friend struct BridgeTestAccess; // implementation exists only in the test binary
 public:
  static Bridge& get();
  void configure(Plan plan, const Sdk& sdk);
  int initialize(const char* versions, char* error);
  void shutdown();
  bool ready() const { return initialized_; } // only while holding mutex
  const Sdk& sdk() const;
  std::uint32_t appid() const { return plan_.appid; }
  std::int32_t pipe() const { return initialized_ ? pipe_ : 0; }
  std::int32_t user() const { return initialized_ ? user_ : 0; }
  GuestAddr interface(std::string_view version);
  void* resolve(GuestAddr handle, std::string_view classname);
  bool next_callback(std::int32_t pipe, void* guest_message);
  void free_callback(std::int32_t pipe);
  bool call_result(std::int32_t pipe, std::uint64_t call, void* data, std::int32_t size, std::int32_t id, bool* failed);
  void run_frame(std::int32_t pipe);
  void release_thread(bool exiting = false);
  std::recursive_mutex mutex;
 private:
  void require_pipe(std::int32_t);
  void* host_interface(const Interface&);
  struct Handle { void* host; const Interface* type; std::uint64_t generation; };
  Plan plan_;
  const Sdk* sdk_{};
  bool initialized_{};
  void* client_{};
  std::int32_t pipe_{}, user_{};
  std::uint64_t generation_{};
  std::map<GuestAddr, std::unique_ptr<Handle>> handles_;
  std::vector<std::byte> callback_;
  std::set<std::pair<std::int32_t, std::int32_t>> unsupported_callbacks_;
  bool outstanding_{};
  std::thread::id callback_thread_;
  bool (*get_callback_)(std::int32_t, HostCallback*, std::uint64_t*){};
  void (*free_callback_)(std::int32_t){};
  bool (*get_result_)(std::int32_t, std::uint64_t, void*, std::int32_t, std::int32_t, bool*){};
  void (*release_tls_)(bool){};
  void (*load_module_)(){};
  void* (*module_export_)(const char*){};
};
class Call {
  std::unique_lock<std::recursive_mutex> lock_;
 public:
  Bridge& bridge;
  void* self;
  Call(GuestAddr handle, std::string_view type) : lock_(Bridge::get().mutex), bridge(Bridge::get()), self(bridge.resolve(handle,type)) {}
};
// A temporary packed record adapter. Arrays use their explicit element count.
class Record {
  const Layout& layout_;
  void* guest_;
  std::size_t count_;
  std::vector<std::byte> host_;
 public:
  Record(const Sdk&, std::string_view type, void* guest, std::size_t count = 1, bool read = true);
  void* data() { return guest_ ? host_.data() : nullptr; }
  void write_back();
};
void release_steam_thread();
void return_record(hle::CallContext&, const Sdk&, std::string_view, const void* host, std::size_t size);
} // namespace venture::steam
