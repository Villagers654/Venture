#include "java/natives/path_bridge.h"
#include "core/process_paths.h"
namespace venture::java {
std::optional<std::string> framework_path_to_guest(std::string_view path) {
  return host_path_to_guest(path);
}
}
