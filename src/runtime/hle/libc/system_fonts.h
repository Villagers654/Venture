// A Quest's /system/fonts and /system/etc/fonts.xml, made of the host's installed
// fonts. Engines fall back to these for glyphs their own fonts lack: Unity reads
// fonts.xml (through /etc) and opens its families' files, Unreal's GameActivity
// checks that /system/fonts is a directory and loads DroidSans.ttf from it as the
// system font. Each file is an installed font under its Android name, opened in
// place and never copied; fonts.xml lists the families and fallbacks of an
// Android 12 fonts.xml for the files this host has.
#pragma once
#include "common/system_fonts.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace venture::hle::libc::system_fonts {
inline constexpr std::string_view directory = "/system/fonts";
inline constexpr std::string_view etc_directory = "/system/etc";
inline constexpr std::string_view fonts_xml_path = "/system/etc/fonts.xml";
struct File {
  std::string name;              // under /system/fonts
  venture::system_fonts::Face host;
};
struct Catalog {
  std::vector<File> files;
  std::string fonts_xml;
  const File* find(std::string_view name) const;
};
using Finder = std::function<std::optional<venture::system_fonts::Face>(const venture::system_fonts::Query&)>;
// The catalog of the faces `find` gives; a font with no face is left out.
Catalog build(const Finder& find);
// The catalog of this host's installed fonts.
Catalog host_catalog();
// This process's catalog, built from the host's fonts on first use.
std::shared_ptr<const Catalog> catalog();
// Replaces the process's catalog (tests).
void set_catalog(Catalog catalog);
// The host file of a guest path under /system/fonts, or empty.
std::filesystem::path host_file(std::string_view guest_path);
// Paths these synthesize: the two directories, fonts.xml and the font files.
bool is_virtual(std::string_view guest_path);
}  // namespace venture::hle::libc::system_fonts
