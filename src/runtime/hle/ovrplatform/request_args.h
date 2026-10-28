#pragma once
#include <functional>
#include <map>
#include <span>
#include <variant>

#include "hle/ovrplatform/provider.h"

namespace venture::ovrplatform {
enum class ArgKind {
  string,
  id,
  size,
  i64,
  u32,
  i32,
  boolean,
  enumeration,
  string_array,
  id_array,
  bytes,
  options,
  model,
  unsupported
};
struct ArgDescriptor {
  ArgKind kind;
  int count_index = -1;
  std::string_view tag{};
};
struct RequestArgumentSpec {
  const char* name;
  std::vector<ArgDescriptor> arguments;
};
std::span<const RequestArgumentSpec> request_argument_specs();
struct EnumValue {
  std::int32_t value;
};
struct OptionsSnapshot {
  std::string kind;
  std::map<std::string, std::vector<std::string>, std::less<>> fields;
};
struct PageSnapshot {
  std::string kind, next_url, previous_url;
};
using RequestArg =
    std::variant<std::string, std::uint64_t, std::int64_t, std::uint32_t, std::int32_t, bool,
                 EnumValue, std::vector<std::string>, std::vector<Id>, std::vector<std::byte>,
                 OptionsSnapshot, PageSnapshot>;
struct RequestArgs {
  std::vector<RequestArg> values;  // SDK declaration order, including counts
  Error error;
};
class Session;
// Scalar bits have already been read using the caller's ABI. Only pointer-array
// stride depends on ISA; all values handed to workers own their storage.
RequestArgs snapshot_request_args(Session&, const RequestArgumentSpec&,
                                  std::span<const std::uint64_t> bits, unsigned pointer_bytes);
}  // namespace venture::ovrplatform
