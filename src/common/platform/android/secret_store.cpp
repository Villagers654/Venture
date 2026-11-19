// SPDX-License-Identifier: GPL-3.0-or-later
// Meta sessions on Android are sealed with an AES-256-GCM key the Android
// Keystore holds and never releases (the app's Keys class). The account key is
// bound into the seal, so a session file copied into another slot does not
// open. Without the app's Java side (a process the Venture app did not start)
// nothing is kept, and nothing is ever written in the clear.
#include "common/secret_store.h"

#include "common/json.h"
#include "common/meta/accounts.h"
#include "common/platform/android/jni.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <stdexcept>

namespace venture {
namespace {
namespace jni = venture::android;
constexpr std::string_view kContext = "venture-meta-session-v1\n";

std::vector<unsigned char> call_keys(const char* method, std::span<const unsigned char> input, std::string_view key) {
  if (!jni::ready() || !jni::classes().keys) throw std::runtime_error("The Android Keystore is not reachable.");
  JNIEnv* env = jni::env();
  jni::LocalFrame frame(env);
  const auto keys = jni::classes().keys;
  jmethodID id = env->GetStaticMethodID(keys, method, "([B[B)[B");
  if (!id) {
    env->ExceptionClear();
    throw std::runtime_error("The Venture app's Keystore bridge does not match this build.");
  }
  std::string context(kContext);
  context += key;
  auto out = static_cast<jbyteArray>(env->CallStaticObjectMethod(
      keys, id, jni::java_bytes(env, input),
      jni::java_bytes(env, {reinterpret_cast<const unsigned char*>(context.data()), context.size()})));
  if (!jni::take_exception(env).empty() || !out) return {};
  return jni::bytes(env, out);
}

void write_private(const fs::path& file, std::span<const unsigned char> bytes) {
  fs::create_directories(file.parent_path());
  auto temp = fs::path(file).concat(".tmp");
  int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) throw std::runtime_error("Could not save Meta session.");
  bool ok = ::write(fd, bytes.data(), bytes.size()) == ssize_t(bytes.size()) && ::fsync(fd) == 0;
  ::close(fd);
  if (!ok || ::rename(temp.c_str(), file.c_str()) != 0) {
    ::unlink(temp.c_str());
    throw std::runtime_error("Could not save Meta session.");
  }
}
}  // namespace

SecretStore::SecretStore(fs::path directory, std::string key) : key_(std::move(key)) {
  if (!meta::valid_account_key(key_)) throw std::runtime_error("Invalid account key.");
  file_ = std::move(directory) / "accounts" / key_ / "session.sealed";
}
std::optional<Session> SecretStore::load() const {
  std::ifstream in(file_, std::ios::binary);
  if (!in) return std::nullopt;
  std::vector<unsigned char> sealed((std::istreambuf_iterator<char>(in)), {});
  auto plain = call_keys("unseal", sealed, key_);
  if (plain.empty()) throw std::runtime_error("Could not unlock Meta session.");
  auto j = parse_json(std::string(plain.begin(), plain.end()));
  if (number(j, "version") != (key_.empty() ? 1 : 2) || str(j, "accessToken").empty() ||
      (!key_.empty() && str(j, "key") != key_))
    throw std::runtime_error("Account credential does not match its slot.");
  return Session{str(j, "accessToken"), str(j, "accountId"), str(j, "alias"),       str(j, "savedAt"),
                 str(j, "source"),      str(j, "profileId"), str(j, "displayName")};
}
void SecretStore::save(const Session& s) const {
  const auto plain = Json{{"version", key_.empty() ? 1 : 2},
                          {"key", key_},
                          {"accessToken", s.access_token},
                          {"accountId", s.account_id},
                          {"alias", s.alias},
                          {"displayName", s.display_name},
                          {"profileId", s.profile_id},
                          {"savedAt", paths::timestamp()},
                          {"source", s.source}}
                         .dump();
  auto sealed = call_keys("seal", {reinterpret_cast<const unsigned char*>(plain.data()), plain.size()}, key_);
  if (sealed.empty()) throw std::runtime_error("Could not save Meta session securely.");
  write_private(file_, sealed);
}
void SecretStore::clear() const {
  std::error_code ignored;
  fs::remove(file_, ignored);
}
const char* SecretStore::protection() { return "sealed with a key the Android Keystore keeps"; }
}  // namespace venture
