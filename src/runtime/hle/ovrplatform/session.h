#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "hle/ovrplatform/provider.h"
#include "hle/ovrplatform/request_args.h"
#include "common/launch_details.h"
#include "common/ipc.h"

namespace venture::ovrplatform {
struct Memory {
  std::function<Handle(std::size_t)> allocate;
  std::function<void(Handle)> free;
};
struct RequestSpec {
  const char* name;
  std::uint32_t type;
};
std::span<const RequestSpec> request_specs();
std::string_view message_type_name(std::uint32_t type);
using RequestHandler = std::function<Result(Provider&, const RequestArgs&, std::stop_token)>;
struct HandlerSpec {
  RequestHandler handler;
  std::string payload_kind;
};
struct StartupRequestState {
  Request id = 0;
  std::uint32_t type = 0;
  bool host = false, received = false, queued = false, popped = false, taken_by_host = false;
  int error = 0;
};
struct StartupPlatformState {
  bool lock_available = false;
  int state = -1;
  std::uint64_t polls = 0;
  std::size_t pending = 0, queued = 0, dropped = 0;
  std::vector<StartupRequestState> requests;
};

// All handles point to small guest allocations, never C++ objects. Strings are
// copied into the same graph and live until FreeMessage (launch details live
// until shutdown). Accessors validate handle kind before using host state.
class Session {
 public:
  Session(AppIdentity, std::shared_ptr<Provider>, Memory);
  ~Session();
  Session(const Session&) = delete;
  Session& operator=(const Session&) = delete;
  InitResult initialize(std::string_view app_id);
  Request initialize_async(std::string_view app_id, std::uint32_t type = 0x1ad307b4);
  Request reject_initialization(std::uint32_t type);
  bool initialized() const;
  Id logged_in_user_id() const;
  void set_microphone_mute(VoipMuteState);
  VoipMuteState microphone_mute() const;
  Request request(const RequestSpec&);
  Request request(const RequestSpec&, RequestArgs);
  Request request_host(const RequestSpec&, RequestArgs);
  void register_handler(std::string_view name, HandlerSpec);
  void notify(std::uint32_t type, Model);  // unsolicited, request ID 0
  Handle pop();
  // Host front ends consume only their own completed request, without building
  // guest handle graphs or stealing messages from the C API queue.
  std::optional<Result> take_result(Request);
  void free_message(Handle);
  std::uint64_t get(Handle, std::string_view accessor) const;
  std::uint64_t get(Handle, std::string_view accessor, std::uint64_t index_or_key) const;
  Handle launch_details();
  void set_launch_details(const LaunchDetails&); // before first borrowed handle
  void attach_launcher(std::shared_ptr<PipeChannel>);
  Handle locale();  // borrowed until shutdown
  Handle message_type_string(std::uint32_t);  // borrowed until shutdown
  Handle enum_string(std::string_view);  // borrowed SDK constant until shutdown
  Handle create_options(std::string_view kind);
  void destroy_options(Handle, std::string_view kind);
  // Values are already snapshotted from guest memory by the facade.
  void set_option(Handle, std::string_view kind, std::string_view field,
                  std::vector<std::string> values, bool append = false);
  void clear_options(Handle, std::string_view kind, std::string_view field);
  std::optional<OptionsSnapshot> snapshot_options(Handle, std::string_view kind) const;
  std::optional<PageSnapshot> snapshot_page(Handle, std::string_view kind) const;
  void shutdown();  // exactly one cancellation per outstanding request
  StartupPlatformState startup_snapshot() const; // metadata only, never payloads or identity
  void log_startup_snapshot() const;
 private:
  Request enqueue(const RequestSpec&, RequestArgs, bool host);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace venture::ovrplatform
