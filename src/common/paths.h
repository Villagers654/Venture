#pragma once
#include "common/json.h"
#include <filesystem>

namespace venture {
namespace fs = std::filesystem;
namespace paths {
fs::path data_dir();
// Atomic default-directory migration; explicit roots make tests independent of
// the owner's profile. Failure returns legacy without copying or deleting data.
fs::path migrate_data_dir(const fs::path &current, const fs::path &legacy);
fs::path executable();
// What a command that starts Venture again names (a Steam shortcut, the
// oculus:// sign-in handler, a helper that outlives this process): the
// AppImage file ($APPIMAGE) when Venture runs from one outside a Flatpak,
// since executable() is then inside the image's mount, a new temporary folder
// each run that is gone once Venture exits; executable() otherwise.
fs::path launch_program();
// UTF-8 <-> host path conversions that never throw. libc++ std::filesystem
// throws runtime_error("locale not supported") for a byte string that is not
// UTF-8 (path(u8string)) and for a Windows name holding an unpaired surrogate
// (u8string()). A guest file name is any byte string, and that exception once
// escaped libc in the middle of a game's frame. On Windows a byte outside a
// UTF-8 sequence becomes U+F000+byte (as host_name does for NTFS-forbidden
// characters; guest_name reverses both), and CESU-8 surrogates (Java's
// modified UTF-8) are accepted. utf8() writes U+FFFD for an unpaired surrogate,
// so its result is always valid UTF-8. POSIX paths stay byte strings.
std::string utf8(const fs::path &path);
// utf8() with '/' separators (generic_u8string without the exception).
std::string generic_utf8(const fs::path &path);
fs::path from_utf8(std::string_view text);
// UTF-16 (a Java string's chars) from UTF-8 with the same lenient decoding.
std::u16string utf16(std::string_view text);
// A guest (Linux) file name component as a host one: on Windows the characters NTFS
// forbids (:*?"<>| and controls) become U+F000+c, as Cygwin, MSYS and WSL store them;
// unchanged elsewhere. guest_name reverses it for directory listings.
std::string host_name(std::string_view guest);
std::string guest_name(std::string_view host);
bool safe_name(std::string_view name);
bool safe_package(std::string_view name);
bool within(const fs::path &root, const fs::path &target);
// An installation's folder name under its package folder: a version code, or
// "<version code>-<binary id>" for a version pinned beside the others
// (meta::Plan::folder).
bool version_folder(std::string_view name);
// What --launch names (and a Steam shortcut launches): a package, or
// "<package>@<version folder>" for a version pinned beside the game's own.
bool safe_launch_target(std::string_view target);
// Reject symlinks/junctions escaping the selected root before
// installing/removing. `version` is a version_folder name.
fs::path game_dir(const fs::path &root, std::string_view package,
                  std::string_view version);
// Path for standard-library/native file I/O: extended namespace on Windows,
// unchanged on POSIX. Keeps platform path rules out of cache callers.
fs::path io_path(const fs::path &file);
std::string read(const fs::path &file, size_t limit = 16 * 1024 * 1024);
void atomic_write(const fs::path &file, std::string_view bytes);
void replace_file(const fs::path &source, const fs::path &destination);
Json read_json(const fs::path &file);
std::string timestamp();
} // namespace paths
} // namespace venture
