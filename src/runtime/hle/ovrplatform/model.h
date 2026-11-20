#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace venture::ovrplatform {
struct Model;
using Blob = std::vector<std::byte>;
// Nested nodes are immutable, owned host data. No guest pointers enter a result.
using ModelValue =
    std::variant<std::uint64_t, std::int64_t, std::uint32_t, std::int32_t, bool, float, double,
                 std::string, Blob, std::vector<std::uint64_t>, std::shared_ptr<const Model>,
                 std::vector<std::string>>;
struct Model {
  std::string kind{};
  std::map<std::string, ModelValue, std::less<>> fields{};
  std::vector<Model> elements{};
};
enum class GetterType { scalar, string, blob, model, f32, f64, id_array, string_array };
enum class GetterOp {
  field,
  element,
  size,
  indexed,
  indexed_size,
  has_next,
  has_previous,
  dict_key,
  dict_value,
  dict_contains,
  dict_size
};
struct ModelGetter {
  const char *name, *model, *field;
  GetterType type;
  GetterOp op;
  const char* nested;
};
std::span<const ModelGetter> model_getters();
const ModelGetter* model_getter(std::string_view name);
Model user_model(std::uint64_t id, std::string alias, std::string display_name);
// Signed Meta Platform 1.205: USER literal RVA 0x896880, current_room
// appended by initializer 0x70980. Graph returns only the id for a user node
// asked without fields, so every user read names them.
inline constexpr const char* user_fields =
    "id,alias,display_name,presence,presence_deeplink_message,"
    "presence_destination_api_name,presence_lobby_session_id,"
    "presence_match_session_id,presence_status,profile_url,profile_url_small,current_room";
}  // namespace venture::ovrplatform
