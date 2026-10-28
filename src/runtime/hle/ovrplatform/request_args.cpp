#include "hle/ovrplatform/request_args.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <stdexcept>

#include "core/guest.h"
#include "hle/ovrplatform/session.h"
#include "platform/memory.h"

namespace venture::ovrplatform {
namespace {
const RequestArgumentSpec specs[] = {
#define VENTURE_REQUEST_ARGS(name, ...) {name, {__VA_ARGS__}},
#include "hle/ovrplatform/request_args.inc"
#undef VENTURE_REQUEST_ARGS
};
void read(GuestAddr address, void* out, std::size_t size, unsigned pointer_bytes) {
  if (!size) return;
  if (!address || address + size < address ||
      (pointer_bytes == 4 && (address > UINT32_MAX || size > (1ull << 32) - address)) ||
      !platform::read_memory(address, out, size))
    throw std::invalid_argument("invalid Platform memory");
}
std::string copy_string(GuestAddr address, unsigned pointer_bytes) {
  if (!address) throw std::invalid_argument("null Platform string");
  constexpr std::size_t limit = 64 * 1024;
  std::string result;
  std::array<char, 256> bytes;
  while (result.size() < limit) {
    const auto at = address + result.size();
    auto n = std::min<std::size_t>(bytes.size(), kGuestPageSize - at % kGuestPageSize);
    read(at, bytes.data(), n, pointer_bytes);
    auto* end = static_cast<const char*>(std::memchr(bytes.data(), 0, n));
    result.append(bytes.data(), end ? end - bytes.data() : n);
    if (end) return result;
  }
  throw std::invalid_argument("oversized Platform string");
}
}  // namespace
std::span<const RequestArgumentSpec> request_argument_specs() { return specs; }
RequestArgs snapshot_request_args(Session& session, const RequestArgumentSpec& spec,
                                  std::span<const std::uint64_t> bits, unsigned pointer_bytes) {
  RequestArgs out;
  if (bits.size() != spec.arguments.size() || (pointer_bytes != 4 && pointer_bytes != 8)) {
    out.error = failure(ErrorCode::invalid_argument);
    return out;
  }
  try {
    for (std::size_t i = 0; i < bits.size(); ++i) {
      const auto& d = spec.arguments[i];
      auto b = bits[i];
      auto count = [&]() -> std::size_t {
        if (d.count_index < 0 || static_cast<std::size_t>(d.count_index) >= bits.size())
          throw std::invalid_argument("Platform buffer lacks count");
        const auto& c = spec.arguments[d.count_index];
        auto n = c.kind == ArgKind::i32
                     ? static_cast<std::uint64_t>(static_cast<std::int32_t>(bits[d.count_index]))
                 : c.kind == ArgKind::u32 ? static_cast<std::uint32_t>(bits[d.count_index])
                                          : bits[d.count_index];
        if (n > (d.kind == ArgKind::bytes ? 4 * 1024 * 1024 : 4096) || (!b && n))
          throw std::invalid_argument("invalid Platform buffer length");
        return static_cast<std::size_t>(n);
      };
      switch (d.kind) {
        case ArgKind::string:
          out.values.emplace_back(copy_string(b, pointer_bytes));
          break;
        case ArgKind::id:
        case ArgKind::size:
          out.values.emplace_back(b);
          break;
        case ArgKind::i64:
          out.values.emplace_back(static_cast<std::int64_t>(b));
          break;
        case ArgKind::u32:
          out.values.emplace_back(static_cast<std::uint32_t>(b));
          break;
        case ArgKind::i32:
          out.values.emplace_back(static_cast<std::int32_t>(b));
          break;
        case ArgKind::enumeration:
          out.values.emplace_back(EnumValue{static_cast<std::int32_t>(b)});
          break;
        case ArgKind::boolean:
          out.values.emplace_back((b & 0xff) != 0);
          break;
        case ArgKind::bytes: {
          std::vector<std::byte> value(count());
          read(b, value.data(), value.size(), pointer_bytes);
          out.values.emplace_back(std::move(value));
          break;
        }
        case ArgKind::id_array: {
          std::vector<Id> value(count());
          read(b, value.data(), value.size() * sizeof(Id), pointer_bytes);
          out.values.emplace_back(std::move(value));
          break;
        }
        case ArgKind::string_array: {
          std::vector<std::string> value;
          auto n = count();
          std::size_t total = 0;
          for (std::size_t j = 0; j < n; ++j) {
            std::uint64_t address = 0;
            read(b + j * pointer_bytes, &address, pointer_bytes, pointer_bytes);
            // Meta Platform 1.205 IAP builder 0x154fe0 skips null SKU entries.
            // Other string-array APIs retain their strict pointer validation.
            if (!address && std::string_view(spec.name) == "ovr_IAP_GetProductsBySKU") continue;
            value.push_back(copy_string(address, pointer_bytes));
            total += value.back().size();
            if (total > 4 * 1024 * 1024)
              throw std::invalid_argument("oversized Platform string array");
          }
          out.values.emplace_back(std::move(value));
          break;
        }
        case ArgKind::options: {
          // OVR_Requests_Application.h makes deeplink_options optional;
          // Unity SDK Scripts/InviteOptions.cs maps null to no suggestions.
          if (!b && (std::string_view(spec.name) == "ovr_Application_LaunchOtherApp" ||
                     d.tag == "InviteOptions")) {
            out.values.emplace_back(OptionsSnapshot{std::string(d.tag), {}});
            break;
          }
          auto value = session.snapshot_options(b, d.tag);
          if (!value) throw std::invalid_argument("invalid Platform options");
          out.values.emplace_back(std::move(*value));
          break;
        }
        case ArgKind::model: {
          auto value = session.snapshot_page(b, d.tag);
          if (!value) throw std::invalid_argument("invalid Platform page");
          out.values.emplace_back(std::move(*value));
          break;
        }
        case ArgKind::unsupported:
          out.error = unsupported_operation(spec.name);
          return out;
      }
    }
  } catch (const std::invalid_argument&) {
    out.error = failure(ErrorCode::invalid_argument);
  }
  return out;
}
}  // namespace venture::ovrplatform
