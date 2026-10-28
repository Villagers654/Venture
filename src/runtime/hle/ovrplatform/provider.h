#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hle/ovrplatform/abi.h"
#include "hle/ovrplatform/model.h"

namespace venture::ovrplatform {
// SDK error codes are service errors, not errno. Venture reserves this range
// so games receive a stable, explicit error without impersonating a Meta code.
enum class ErrorCode : std::int32_t {
  none = 0,
  unsupported = -20000,
  unauthenticated = -20001,
  not_entitled = -20002,
  unavailable = -20003,
  invalid_response = -20004,
  identity_mismatch = -20005,
  cancelled = -20006,
  uninitialized = -20007,
  incomplete = -20008,
  invalid_argument = -20009,
};
struct Error {
  ErrorCode code = ErrorCode::none;
  std::int32_t http_status = 0;
  std::string message;
  explicit operator bool() const { return code != ErrorCode::none; }
};
Error failure(ErrorCode code);
Error unsupported_operation(std::string_view name);
std::string host_locale();  // platform/{win32,posix}, BCP-47
struct User {
  Id id = 0;
  std::string alias, display_name;
};
struct Result {
  Error error;
  User user{};
  std::string text{};
  Model model{};  // additive; legacy providers' User/text are adapted by the registry
  // Enqueue first, then wait separately so cancellation can still run.
  std::function<Result(std::stop_token)> deferred{};
};
struct AppIdentity {
  std::string app_id, package, binary_id;
  std::int64_t version_code = 0;
  std::string version_name{};
};
// plan.json is identity input, never entitlement evidence. The runtime passes
// the APK's independently parsed package/version to prevent a mismatched plan.
AppIdentity read_app_identity(const std::filesystem::path& plan, std::string_view apk_package,
                              std::int64_t apk_version);
bool valid_id(std::string_view text);

struct Credential {
  std::string access_token, account_id;
  bool operator==(const Credential&) const = default;
};
class SecretStore {
 public:
  virtual ~SecretStore() = default;
  virtual Credential load() = 0;  // throws; callers redact all exception details
  virtual std::filesystem::path platform_directory(std::string_view) const { return {}; }
};
// Portable account-vault adapter. Does not write or modify launcher credentials.
// A launcher SecretStore adapter can be injected.
std::shared_ptr<SecretStore> runtime_secret_store();
std::shared_ptr<SecretStore> runtime_secret_store(std::string_view account_key);
struct LaunchCredentials {
  std::shared_ptr<SecretStore> identity_store, entitlement_store;
};

using Fields = std::vector<std::pair<std::string, std::string>>;
enum class HttpMethod { get, post, erase };
enum class HttpShape { legacy_form, graph_query, unversioned_query };
struct Endpoint {
  std::string name;
  HttpMethod method;
  std::string path;  // numeric {app}/{id} templates only, never guest URLs
  std::vector<std::string> parameters;
  HttpShape shape = HttpShape::graph_query;
  bool read_only = false;  // POST reads must explicitly opt in; writes never retry
};
struct HttpRequest {
  std::string endpoint;
  Fields parameters{};
  std::string bearer{}, app_id{}, id{}, next_url{};
  bool retry_reads = true;  // bounded diagnostics can require exactly one wire read
};
struct HttpResponse {
  std::int32_t status = 0;
  std::string body;
  Error error;
};
class HttpTransport {
 public:
  virtual ~HttpTransport() = default;
  virtual HttpResponse post(std::string_view path, const Fields&, std::stop_token) = 0;
  virtual HttpResponse get(std::string_view path, const Fields&, std::stop_token) = 0;
  virtual HttpResponse send(const HttpRequest&, std::stop_token) {
    return {0, {}, failure(ErrorCode::unsupported)};
  }
  virtual void register_endpoint(Endpoint) {
    throw std::logic_error("Endpoint registration unsupported");
  }
};
bool valid_next_page_url(std::string_view url);
Error http_response_error(const HttpResponse&);  // never carries server text
// Production is locked to Meta HTTPS; the separately named loopback factory is
// for integration tests and cannot receive the real session by default.
std::shared_ptr<HttpTransport> meta_http_transport();
std::shared_ptr<HttpTransport> loopback_http_transport(std::uint16_t port);

struct RequestArgs;
class Provider {
 public:
  virtual ~Provider() = default;
  virtual Error prepare() = 0;
  // The app id the game declared at initialization, before prepare(); later
  // requests (and Meta's answers) are for it.
  virtual void declare_app_id(std::string_view) {}
  virtual Result entitlement(std::stop_token) = 0;
  virtual Result logged_in_user(std::stop_token) { return {failure(ErrorCode::unsupported)}; }
  virtual Result access_token(std::stop_token) { return {failure(ErrorCode::unsupported)}; }
  virtual Result user_proof(std::stop_token) { return {failure(ErrorCode::unsupported)}; }
  // rich_presence_options is owned JSON, never a guest pointer. No retries.
  virtual Result heartbeat(std::string_view, std::stop_token) { return {failure(ErrorCode::unsupported)}; }
  virtual Result service(std::string_view name, const RequestArgs&, std::stop_token) {
    return {unsupported_operation(name)};
  }
  // Host feature handlers use registered endpoints with the launch account's
  // app token. This also propagates Graph auth revocation to the shared cache.
  virtual HttpResponse graph_request(HttpRequest, std::stop_token) {
    return {0, {}, failure(ErrorCode::unsupported)};
  }
};
std::shared_ptr<Provider> owned_library_provider(
    AppIdentity, std::shared_ptr<SecretStore>, std::shared_ptr<HttpTransport>);
std::shared_ptr<Provider> owned_library_provider(
    AppIdentity, LaunchCredentials, std::shared_ptr<HttpTransport>);

// Separate feasibility diagnostic. Never exposes candidate tokens to guest
// code; success means only that Meta returned a candidate, not proven scope.
struct AuthProbe {
  std::int32_t exchange_status = 0, identity_status = 0, scope_status = 0;
  bool token_returned = false, identity_returned = false;
  bool token_differs_from_profile = false, identity_matches_profile = false;
  bool app_scope_confirmed = false;
  Error error;
};
AuthProbe probe_application_auth(const AppIdentity&, SecretStore&, HttpTransport&,
                                 std::stop_token = {});
}  // namespace venture::ovrplatform
