#include "common/secret_store.h"
#include "common/meta/accounts.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#ifdef VENTURE_HAVE_LIBSECRET
// Types and constants only: libsecret is loaded at run time, so the release
// does not require it (or GLib) to be installed.
#include <libsecret/secret.h>
#endif
namespace venture {
SecretStore::SecretStore(fs::path directory, std::string key)
    : key_(std::move(key)) {
  if (!meta::valid_account_key(key_))
    throw std::runtime_error("Invalid account key.");
  file_ = std::move(directory) / "accounts" / key_ / "session.dpapi";
}
namespace {
std::string slot_of(const std::string &key) { return key.empty() ? "legacy" : key; }

// Fallback when no Secret Service is reachable (libsecret absent, no session
// bus or no keyring daemon): AES-256-GCM in a file only the user can read. The
// key comes from a random per-slot key file and the machine id, so a copied
// sealed file alone does not decrypt. This is weaker than a keyring.
fs::path sealed_path(const fs::path &file) { return fs::path(file).replace_extension(".sealed"); }
fs::path key_path(const fs::path &file) { return fs::path(file).replace_extension(".key"); }
constexpr std::string_view sealed_magic = "VSS1";

std::optional<std::string> read_private(const fs::path &file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) return std::nullopt;
  return std::string(std::istreambuf_iterator<char>(in), {});
}
void write_private(const fs::path &file, std::string_view bytes) {
  fs::create_directories(file.parent_path());
  auto temp = fs::path(file).concat(".tmp");
  int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) throw std::runtime_error("Could not save Meta session.");
  ::fchmod(fd, 0600);
  bool ok = ::write(fd, bytes.data(), bytes.size()) == ssize_t(bytes.size()) && ::fsync(fd) == 0;
  ::close(fd);
  if (!ok || ::rename(temp.c_str(), file.c_str()) != 0) {
    ::unlink(temp.c_str());
    throw std::runtime_error("Could not save Meta session.");
  }
}
std::string machine_id() {
  for (auto *path : {"/etc/machine-id", "/var/lib/dbus/machine-id"})
    if (auto id = read_private(path); id && !id->empty()) return *id;
  return {};
}
std::string sealing_key(const fs::path &file, bool create) {
  auto random = read_private(key_path(file));
  if (!random || random->size() != 32) {
    if (!create) return {};
    std::string fresh(32, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(fresh.data()), 32) != 1)
      throw std::runtime_error("Could not save Meta session.");
    write_private(key_path(file), fresh);
    random = fresh;
  }
  std::string material = "venture-sealed-session-v1\n" + *random + machine_id();
  std::string key(32, '\0');
  unsigned int size = 0;
  if (!EVP_Digest(material.data(), material.size(), reinterpret_cast<unsigned char *>(key.data()),
                  &size, EVP_sha256(), nullptr) || size != 32)
    throw std::runtime_error("Could not protect Meta session.");
  return key;
}
std::string gcm(bool encrypt, std::string_view key, std::string_view nonce,
                std::string_view aad, std::string_view input, std::string_view tag_in,
                std::string *tag_out) {
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(),
                                                                       EVP_CIPHER_CTX_free);
  auto u = [](std::string_view s) { return reinterpret_cast<const unsigned char *>(s.data()); };
  std::string out(input.size(), '\0');
  int n = 0, tail = 0;
  bool ok = ctx && EVP_CipherInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, u(key), u(nonce), encrypt) == 1 &&
            EVP_CipherUpdate(ctx.get(), nullptr, &n, u(aad), int(aad.size())) == 1 &&
            EVP_CipherUpdate(ctx.get(), reinterpret_cast<unsigned char *>(out.data()), &n, u(input),
                             int(input.size())) == 1;
  if (ok && !encrypt)
    ok = EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16,
                             const_cast<char *>(tag_in.data())) == 1;
  ok = ok && EVP_CipherFinal_ex(ctx.get(), reinterpret_cast<unsigned char *>(out.data()) + n, &tail) == 1;
  if (ok && encrypt) {
    tag_out->assign(16, '\0');
    ok = EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, tag_out->data()) == 1;
  }
  if (!ok) throw std::runtime_error("Could not unlock Meta session.");
  return out;
}
void seal(const fs::path &file, const std::string &slot, std::string_view plain) {
  auto key = sealing_key(file, true);
  std::string nonce(12, '\0'), tag;
  if (RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), 12) != 1)
    throw std::runtime_error("Could not save Meta session.");
  auto cipher = gcm(true, key, nonce, slot, plain, {}, &tag);
  write_private(sealed_path(file), std::string(sealed_magic) + nonce + cipher + tag);
}
std::optional<std::string> unseal(const fs::path &file, const std::string &slot) {
  auto data = read_private(sealed_path(file));
  if (!data) return std::nullopt;
  auto key = sealing_key(file, false);
  if (key.empty() || data->size() < sealed_magic.size() + 12 + 16 || !data->starts_with(sealed_magic))
    throw std::runtime_error("Could not unlock Meta session.");
  std::string_view view(*data);
  view.remove_prefix(sealed_magic.size());
  return gcm(false, key, view.substr(0, 12), slot, view.substr(12, view.size() - 28),
             view.substr(view.size() - 16), nullptr);
}
void remove_sealed(const fs::path &file) {
  std::error_code ec;
  fs::remove(sealed_path(file), ec);
  fs::remove(key_path(file), ec);
}

#ifdef VENTURE_HAVE_LIBSECRET
// Persisted schema must continue matching existing Secret Service entries.
constexpr auto legacy_schema_name = "org.questport.MetaSession";
const SecretSchema schema = {legacy_schema_name,
                             SECRET_SCHEMA_NONE,
                             {{"root", SECRET_SCHEMA_ATTRIBUTE_STRING},
                              {"slot", SECRET_SCHEMA_ATTRIBUTE_STRING},
                              {nullptr, {}}}};
struct Libsecret {
  gchar *(*lookup)(const SecretSchema *, GCancellable *, GError **, ...) = nullptr;
  gboolean (*store)(const SecretSchema *, const gchar *, const gchar *, const gchar *,
                    GCancellable *, GError **, ...) = nullptr;
  gboolean (*clear)(const SecretSchema *, GCancellable *, GError **, ...) = nullptr;
  void (*free_password)(gchar *) = nullptr;
  void (*free_error)(GError *) = nullptr;
  GQuark (*dbus_quark)() = nullptr;
  GQuark (*io_quark)() = nullptr;
  // Unlocking the default collection.
  SecretService *(*service)(SecretServiceFlags, GCancellable *, GError **) = nullptr;
  SecretCollection *(*for_alias)(SecretService *, const gchar *, SecretCollectionFlags, GCancellable *,
                                 GError **) = nullptr;
  gboolean (*collection_locked)(SecretCollection *) = nullptr;
  gint (*unlock)(SecretService *, GList *, GCancellable *, GList **, GError **) = nullptr;
  GList *(*list_append)(GList *, gpointer) = nullptr;
  void (*list_free)(GList *) = nullptr;
  void (*unref)(gpointer) = nullptr;
  bool can_unlock = false;
  bool ok = false;
};
const Libsecret &libsecret() {
  static const Libsecret api = [] {
    Libsecret l;
    void *lib = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return l;
    auto bind = [lib](auto &fn, const char *name) {
      fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
      return fn != nullptr;
    };
    l.ok = bind(l.lookup, "secret_password_lookup_sync") && bind(l.store, "secret_password_store_sync") &&
           bind(l.clear, "secret_password_clear_sync") && bind(l.free_password, "secret_password_free") &&
           bind(l.free_error, "g_error_free") && bind(l.dbus_quark, "g_dbus_error_quark") &&
           bind(l.io_quark, "g_io_error_quark");
    l.can_unlock = bind(l.service, "secret_service_get_sync") &&
                   bind(l.for_alias, "secret_collection_for_alias_sync") &&
                   bind(l.collection_locked, "secret_collection_get_locked") &&
                   bind(l.unlock, "secret_service_unlock_sync") && bind(l.list_append, "g_list_append") &&
                   bind(l.list_free, "g_list_free") && bind(l.unref, "g_object_unref");
    return l;
  }();
  return api;
}
// Outcome of one Secret Service call: unreachable selects the sealed file.
enum class Keyring { ok, unreachable, failed };
// Whether a Secret Service last answered (1) or could not be reached (0); -1 before any call.
std::atomic<int> answered{-1};
Keyring outcome(GError *error) {
  if (!error) {
    answered = 1;
    return Keyring::ok;
  }
  auto &l = libsecret();
  bool unreachable = error->domain == l.dbus_quark() ||
                     (error->domain == l.io_quark() && error->code != G_IO_ERROR_CANCELLED);
  l.free_error(error);
  answered = unreachable ? 0 : 1;
  return unreachable ? Keyring::unreachable : Keyring::failed;
}
bool keyring_answers() {
  if (!libsecret().ok) return false;
  if (answered < 0) {  // probe once: a lookup that matches nothing never prompts
    GError *error = nullptr;
    auto &l = libsecret();
    if (auto *secret = l.lookup(&schema, nullptr, &error, "root", "", "slot", "", nullptr))
      l.free_password(secret);
    outcome(error);
  }
  return answered == 1;
}
std::string root(const fs::path &file, bool legacy) {
  return paths::utf8(
      fs::absolute(legacy ? file.parent_path()
                          : file.parent_path().parent_path().parent_path()));
}
std::string legacy_root(std::string_view location) {
  auto path = paths::from_utf8(location);
  // Secret Service keys include the absolute data root, unlike Windows DPAPI.
  // Look up the released root attribute after an atomic directory migration.
  constexpr auto legacy_name = "questport"; // rebrand: legacy libsecret root attribute
  if (path.filename() == "venture") return paths::utf8(path.parent_path() / legacy_name);
  return std::string(location);
}
// The non-secret marker records the root its Secret Service item was saved
// under. A copied data root (harness seeds, backups) then still resolves its
// credential, as a copied DPAPI blob does on Windows. Only load() follows it:
// clear() in a copy never removes the original's credential.
std::string marker_root(const fs::path &file, std::string_view key) {
  try {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    auto j = parse_json(std::string(std::istreambuf_iterator<char>(in), {}));
    auto value = str(j, "root");
    if (str(j, "key") != key || !paths::from_utf8(value).is_absolute()) return {};
    return value;
  } catch (...) {
    return {};
  }
}
// Unlocks the default collection, which shows the keyring's own password
// prompt, at most once per process. A lookup unlocks the items it finds, but
// KWallet encrypts attributes too, so a closed wallet finds none and never
// prompts.
void unlock_default_collection() {
  static std::atomic_flag tried = ATOMIC_FLAG_INIT;
  auto &l = libsecret();
  // libsecret's file backend (SECRET_BACKEND=file: tests, sandboxes) has no
  // collection to open; the service call would reach the desktop keyring.
  const char *backend = std::getenv("SECRET_BACKEND");
  if (!l.can_unlock || (backend && std::string_view(backend) != "service") || tried.test_and_set()) return;
  GError *error = nullptr;
  SecretService *service = l.service(SECRET_SERVICE_LOAD_COLLECTIONS, nullptr, &error);
  if (!service) {
    if (error) l.free_error(error);
    return;
  }
  if (SecretCollection *collection =
          l.for_alias(service, SECRET_COLLECTION_DEFAULT, SECRET_COLLECTION_NONE, nullptr, &error)) {
    if (l.collection_locked(collection)) {
      GList *objects = l.list_append(nullptr, collection);
      l.unlock(service, objects, nullptr, nullptr, &error);
      l.list_free(objects);
    }
    l.unref(collection);
  }
  if (error) l.free_error(error);
  l.unref(service);
}
// Secret Service lookup; nullopt when absent or unreachable.
std::optional<std::string> keyring_load(const fs::path &file, const std::string &key) {
  auto &l = libsecret();
  if (!l.ok) return std::nullopt;
  auto location = root(file, key.empty()), slot = slot_of(key);
  // Saved to the keyring (a marker without a sealed file): open it first.
  std::error_code ec;
  if (fs::exists(file, ec) && !fs::exists(sealed_path(file), ec)) unlock_default_collection();
  GError *error = nullptr;
  auto *secret = l.lookup(&schema, nullptr, &error, "root", location.c_str(), "slot",
                          slot.c_str(), nullptr);
  if (!secret && !error && legacy_root(location) != location)
    secret = l.lookup(&schema, nullptr, &error, "root", legacy_root(location).c_str(), "slot",
                      slot.c_str(), nullptr);
  if (!secret && !error && !key.empty())
    if (auto origin = marker_root(file, key); !origin.empty() && origin != location)
      secret = l.lookup(&schema, nullptr, &error, "root", origin.c_str(), "slot", slot.c_str(),
                        nullptr);
  switch (outcome(error)) {
  case Keyring::unreachable: return std::nullopt;
  case Keyring::failed: throw std::runtime_error("Could not unlock Meta session.");
  case Keyring::ok: break;
  }
  if (!secret) return std::nullopt;
  std::string plain(secret);
  l.free_password(secret);
  return plain;
}
Keyring keyring_save(const fs::path &file, const std::string &key, const std::string &plain) {
  auto &l = libsecret();
  if (!l.ok) return Keyring::unreachable;
  auto location = root(file, key.empty()), slot = slot_of(key);
  GError *error = nullptr;
  bool ok = l.store(&schema, SECRET_COLLECTION_DEFAULT, "Venture Meta session", plain.c_str(),
                    nullptr, &error, "root", location.c_str(), "slot", slot.c_str(), nullptr);
  auto result = outcome(error);
  return result == Keyring::ok && !ok ? Keyring::failed : result;
}
Keyring keyring_clear(const fs::path &file, const std::string &key) {
  auto &l = libsecret();
  if (!l.ok) return Keyring::unreachable;
  auto location = root(file, key.empty()), slot = slot_of(key);
  GError *error = nullptr;
  l.clear(&schema, nullptr, &error, "root", location.c_str(), "slot", slot.c_str(), nullptr);
  if (!error && legacy_root(location) != location)
    l.clear(&schema, nullptr, &error, "root", legacy_root(location).c_str(), "slot",
            slot.c_str(), nullptr);
  // clear_sync returns FALSE when nothing matched; only errors fail.
  return outcome(error);
}
#else
enum class Keyring { ok, unreachable, failed };
std::optional<std::string> keyring_load(const fs::path &, const std::string &) { return std::nullopt; }
Keyring keyring_save(const fs::path &, const std::string &, const std::string &) { return Keyring::unreachable; }
Keyring keyring_clear(const fs::path &, const std::string &) { return Keyring::unreachable; }
std::string root(const fs::path &file, bool legacy) {
  return paths::utf8(fs::absolute(legacy ? file.parent_path()
                                         : file.parent_path().parent_path().parent_path()));
}
#endif
} // namespace
namespace secret_seal {
void save(const fs::path &file, std::string_view context,
          std::string_view plaintext) {
  try {
    seal(file, std::string(context), plaintext);
  } catch (const std::exception &) {
    throw std::runtime_error("Could not save the sealed local file.");
  }
}
std::optional<std::string> load(const fs::path &file, std::string_view context) {
  try {
    return unseal(file, std::string(context));
  } catch (const std::exception &) {
    throw std::runtime_error("Could not unlock the sealed local file.");
  }
}
void clear(const fs::path &file) {
  remove_sealed(file);
  fs::remove(file);
}
} // namespace secret_seal
std::optional<Session> SecretStore::load() const {
  auto plain = keyring_load(file_, key_);
  if (!plain) plain = unseal(file_, slot_of(key_));
  if (!plain) return {};
  auto j = parse_json(*plain);
  if (number(j, "version") != (key_.empty() ? 1 : 2) ||
      str(j, "accessToken").empty() || (!key_.empty() && str(j, "key") != key_))
    throw std::runtime_error("Account credential does not match its slot.");
  return Session{str(j, "accessToken"), str(j, "accountId"),
                 str(j, "alias"),       str(j, "savedAt"),
                 str(j, "source"),      str(j, "profileId"),
                 str(j, "displayName")};
}
void SecretStore::save(const Session &s) const {
  auto location = root(file_, key_.empty());
  auto plain = Json{
      {"version", key_.empty() ? 1 : 2},
      {"key", key_},
      {"accessToken", s.access_token},
      {"accountId", s.account_id},
      {"alias", s.alias},
      {"displayName", s.display_name},
      {"profileId", s.profile_id},
      {"savedAt", paths::timestamp()},
      {"source",
       s.source}}.dump();
  switch (keyring_save(file_, key_, plain)) {
  case Keyring::ok: remove_sealed(file_); break;
  case Keyring::unreachable: seal(file_, slot_of(key_), plain); break;
  case Keyring::failed: throw std::runtime_error("Could not save Meta session securely.");
  }
  // A non-secret marker allows orphan reconciliation; credentials stay in
  // Secret Service (or the sealed file).
  paths::atomic_write(file_, Json{{"version", 2}, {"key", key_}, {"root", location}}.dump());
}
void SecretStore::clear() const {
  if (keyring_clear(file_, key_) == Keyring::failed)
    throw std::runtime_error("Could not clear Meta session securely.");
  remove_sealed(file_);
  fs::remove(file_);
}
const char *SecretStore::protection() {
#ifdef VENTURE_HAVE_LIBSECRET
  if (keyring_answers()) return "kept in your desktop keyring";
#endif
  return "kept in an encrypted file only your user can read";
}
} // namespace venture
