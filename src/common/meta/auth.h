#pragma once
#include "common/http/client.h"
#include "common/secret_store.h"
#include <chrono>
#include <stdexcept>
namespace venture::meta {
class RequestError : public std::runtime_error {
public:
  RequestError(long status, std::string message) : std::runtime_error(std::move(message)), status(status) {}
  long status;
};
std::string sha256(std::string_view text);
bool auth_navigation_allowed(std::string_view url);
bool is_callback(std::string_view url);
// Returns the opaque blob only after scheme, challenge and size validation.
std::string validate_callback(std::string_view callback,
                              std::string_view challenge);
Json post(std::string_view url, const http::Fields &fields,
          std::stop_token stop = {}, const http::Fields &headers = {});
class Auth {
public:
  std::string begin(std::stop_token stop = {});
  Session complete(std::string_view callback, std::stop_token stop = {});

private:
  std::string challenge_;
  std::chrono::steady_clock::time_point expires_;
};
} // namespace venture::meta
