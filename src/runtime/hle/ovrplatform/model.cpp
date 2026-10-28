#include "hle/ovrplatform/model.h"

#include <algorithm>

namespace venture::ovrplatform {
namespace {
constexpr ModelGetter getters[] = {
#define VENTURE_MODEL_GETTER(name, model, field, type, op, nested) \
  {name, model, field, GetterType::type, GetterOp::op, nested},
#include "hle/ovrplatform/model_getters.inc"
#undef VENTURE_MODEL_GETTER
};
}  // namespace
std::span<const ModelGetter> model_getters() { return getters; }
const ModelGetter* model_getter(std::string_view name) {
  auto it = std::lower_bound(
      std::begin(getters), std::end(getters), name,
      [](const auto& getter, auto name) { return std::string_view(getter.name) < name; });
  return it != std::end(getters) && it->name == name ? it : nullptr;
}
Model user_model(std::uint64_t id, std::string alias, std::string display_name) {
  return {"User",
          {{"ID", id}, {"OculusID", std::move(alias)}, {"DisplayName", std::move(display_name)}}};
}
}  // namespace venture::ovrplatform
