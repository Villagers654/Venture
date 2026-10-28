#include "common/launch_details.h"
#include <stdexcept>

namespace venture {
Json launch_details_json(const LaunchDetails& d) {
  return {{"type", d.type}, {"source", d.source}, {"deeplink", d.deeplink},
          {"destination", d.destination}, {"lobby", d.lobby}, {"match", d.match}, {"room", d.room}};
}
LaunchDetails read_launch_details(const Json& j) {
  if (!j.is_object()) throw std::runtime_error("Invalid launch details");
  LaunchDetails d;
  const auto& type = j.at("type");
  if (!type.is_number_integer() || type < 1 || type > 4)
    throw std::runtime_error("Invalid launch type");
  d.type = type.get<std::int32_t>();
  for (auto [key, out] : {std::pair{"source", &d.source}, {"deeplink", &d.deeplink},
       {"destination", &d.destination}, {"lobby", &d.lobby}, {"match", &d.match}}) {
    *out = j.at(key).get<std::string>();
    if (out->size() > 4096 || out->find('\0') != out->npos)
      throw std::runtime_error("Invalid launch field");
  }
  if (!j.at("room").is_number_unsigned()) throw std::runtime_error("Invalid launch room");
  d.room = j.at("room").get<std::uint64_t>();
  return d;
}
} // namespace venture
