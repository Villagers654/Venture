#pragma once
// Quest mods: mod packages (.qmod) and the patched APKs whose mod loader reads
// them, as the Quest's mod managers (QuestPatcher, ModsBeforeFriday) install
// them.
//
// A .qmod is a ZIP with a mod.json manifest naming its loader, its files and
// where they go, its file copies and the mods it depends on. Its loader reads
// mods from fixed places on the headset's shared storage:
//   Scotland2    /sdcard/ModData/<package>/Modloader/{early_mods,mods,libs}/
//                and the loader itself, /sdcard/ModData/<package>/Modloader/libsl2.so
//   QuestLoader  /sdcard/Android/data/<package>/files/{mods,libs}/
//
// Venture keeps a game's mods in its mods folder (mods_dir), outside every
// installation and account:
//   installed/<id>/   each mod's package contents (mod.json and its files)
//   installed/state.json  which are turned off, and which came as dependencies
//   items/<ext>/      files a mod registered an extension for (copyExtensions)
//   loader/           the loader's own file (libsl2.so)
// At launch sync() publishes the enabled mods' files into the account's storage
// at the paths the loader reads, and takes back what it published before and no
// longer should. Files the game or a mod changed there stay its own.
#include "common/paths.h"
#include <functional>
#include <optional>
#include <stop_token>
#include <vector>

namespace venture::mods {
enum class Loader { scotland2, questloader };
struct Dependency {
  std::string id, range = "*", download;  // download: the manifest's downloadIfMissing
  bool required = true;
};
struct FileCopy {
  std::string name, destination;  // a member of the package, a guest path
};
struct CopyExtension {
  std::string extension, destination;  // without its dot; a guest folder
};
struct Manifest {
  std::string schema, id, name, author, porter, version, description, cover;
  std::string package, package_version;  // empty: any app, any version
  bool library = false;
  Loader loader = Loader::questloader;
  std::vector<std::string> mod_files, late_mod_files, library_files;
  std::vector<FileCopy> copies;
  std::vector<Dependency> dependencies;
  std::vector<CopyExtension> extensions;
};
// A mod.json's manifest; throws a message for the player when it is not one.
Manifest parse_manifest(std::string_view json);
const char *loader_name(Loader loader);

// Semantic versions and the ranges dependencies name (node-semver, as the
// Quest's mod managers read them): "1.2.3", "^1.2.0", "~1.2", ">=1.0.0 <2.0.0",
// "1.x", "*", "1.0.0 - 1.4.0" and alternatives joined by "||".
bool valid_version(std::string_view version);
bool satisfies(std::string_view version, std::string_view range);

// Where a guest storage path ("/sdcard/...", "/storage/emulated/0/...") is in a
// game's app data directory (account_data_dir): external/... or, under
// Android/data/<package>, external-data/.... Empty for paths outside shared
// storage, the OBB folder, or with "..".
fs::path storage_path(std::string_view package, std::string_view guest);

struct Installed {
  Manifest manifest;
  fs::path directory;
  bool enabled = true;
  bool dependency = false;  // installed for another mod, not by the player
};
// A game's installed mods, by name.
std::vector<Installed> list(const fs::path &mods);
// Installs a .qmod for `package`, replacing an installed mod of the same id
// (another version), turned on. Throws for a package that is not a .qmod, is
// for another game, or lists files it does not hold.
Installed install(const fs::path &mods, std::string_view package, const fs::path &qmod, bool dependency = false);
// The manifest of a .qmod, without installing it.
Manifest read_package(const fs::path &qmod);
// Turning a mod on turns on the installed mods it needs; turning one off turns
// off the mods that need it.
void set_enabled(const fs::path &mods, std::string_view id, bool enabled);
// Removes a mod, and library mods nothing installed needs any more.
void remove(const fs::path &mods, std::string_view id);
// The dependencies of `mod` no installed mod satisfies (absent, or another
// version than the range allows); optional ones only when installed at a wrong
// version.
std::vector<Dependency> missing_dependencies(const std::vector<Installed> &installed, const Manifest &mod);

// What install_with_dependencies did: the mods installed (the package first),
// and dependencies left missing, as "<id> <range> [<range>...]".
struct Report {
  std::vector<Installed> installed;
  std::vector<std::string> missing;
};
// Downloads a dependency's package from the URL its manifest names into a file.
using Fetch = std::function<void(const std::string &url, const fs::path &file, std::stop_token stop)>;
// install(), then the dependencies of the package (and theirs) that are
// missing or at a version some enabled mod's range excludes. A dependency's
// version must satisfy the ranges of every enabled mod that needs it: of the
// downloads those mods' manifests link (each fetched once with `fetch`; null:
// none are fetched), the highest version that does is installed, not the
// first one linked. Installed dependencies that satisfy them are turned on.
Report install_with_dependencies(const fs::path &mods, std::string_view package, const fs::path &qmod,
                                 const Fetch &fetch, std::stop_token stop = {});

// Files of a type an enabled mod registered (copyExtensions, such as a saber
// or a playlist): added to the mod's folder for that type.
struct Item {
  std::string extension, name;
  fs::path file;
};
std::vector<Item> items(const fs::path &mods);
// Whether an enabled mod takes files with `file`'s extension.
bool takes_item(const fs::path &mods, const fs::path &file);
Item add_item(const fs::path &mods, const fs::path &file);
void remove_item(const fs::path &mods, std::string_view extension, std::string_view name);

// The mod loader a patched APK carries, from the tag its patcher adds
// (modded.json's modloaderName, or the older QuestLoader tags); nullopt for an
// APK no mod patcher tagged.
std::optional<Loader> apk_loader(const fs::path &apk);
// Scotland2 lives on the headset's storage, not in the APK: its file in the
// mods folder (loader/libsl2.so), empty when absent.
fs::path loader_file(const fs::path &mods);
void set_loader_file(const fs::path &mods, const fs::path &file);
// Where Scotland2's patchers get it: its latest release.
inline constexpr const char *scotland2_url = "https://github.com/sc2ad/scotland2/releases/latest/download/libsl2.so";

// Expansion files (OBB) the player replaces: `file` is written over the
// installed `name` (an "obb" file of the install's plan) in the game's folder.
// The store's file is not kept: library::replaced_file keeps such an install
// complete, and downloading the game again brings the store's file back.
void replace_obb(const fs::path &directory, std::string_view name, const fs::path &file);
// The OBB files an install's plan names, in its order.
std::vector<std::string> obb_names(const fs::path &directory);
// Earlier Venture versions used a same-named file in the mods folder in place
// of the installed expansion file at each launch. Each such file now replaces
// the installed one, once (moved, or copied and removed); returns their names.
std::vector<std::string> adopt_legacy_obbs(const fs::path &install, const std::vector<std::string> &names,
                                           const fs::path &mods);

// A patched APK replaces an installed game's APK in place (`directory`, its
// version folder): same package, same version code, so its expansion files,
// add-ons, saves and settings all stay. The APK it replaces first is kept as
// <apk>.original for restore_apk. False when the install already has this APK.
// Throws for another package or version.
bool replace_apk(const fs::path &directory, const fs::path &apk);
// The kept original APK of an install; empty when there is none.
fs::path original_apk(const fs::path &directory);
// Puts the original APK back.
void restore_apk(const fs::path &directory);

struct SyncResult {
  std::size_t published = 0, removed = 0, kept = 0;  // kept: changed by the game, left as it is
};
// Publishes the enabled mods' files, the items and the loader into a game's app
// data directory (account_data_dir) at the paths its loader reads. Files the
// last sync published and this one does not are removed unless the game
// changed them. Mod libraries are hard links where the drive allows; files a
// mod may rewrite (file copies, items) are copies.
SyncResult sync(const fs::path &mods, std::string_view package, const fs::path &data_dir);
}  // namespace venture::mods
