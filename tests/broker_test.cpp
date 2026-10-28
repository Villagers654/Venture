#include "common/file_ops.h"
#include "common/process.h"
#include <doctest/doctest.h>
#include <thread>

#include "async_wait.h"

using namespace venture;
namespace {
struct Directory {
  fs::path root =
      fs::temp_directory_path() / ("venture-accounts-" + random_account_key());
  Directory() { fs::create_directories(root); }
  ~Directory() {
    try {
      remove_tree(root.parent_path(), root);
    } catch (...) {
    }
  }
};
} // namespace
TEST_CASE("inherited credential pipes keep tokens out of argv environment and "
          "child logs") {
  Directory d;
  Process child;
  child.spawn(VENTURE_BROKER_CHILD, {"--account", "123456abcdef"},
              d.root / "child.log", false, [](const Json &request) {
                return str(request, "op") == "credentials"
                           ? Json{{"appToken", "fixture-app-pipe-secret"}}
                           : Json{{"error", 0}};
              });
  REQUIRE(test::wait_until([&] { return child.poll().has_value(); }));
  CHECK(*child.poll() == 0);
  auto log = paths::read(d.root / "child.log");
  CHECK(log.find("tokenInArgv=0 tokenInEnv=0 tokenFromPipe=1") !=
        std::string::npos);
  CHECK(log.find("fixture-app-pipe-secret") == std::string::npos);
}
