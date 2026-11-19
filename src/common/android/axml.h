#pragma once
// Android binary XML (AOSP ResourceTypes.h, as aapt/aapt2 write
// AndroidManifest.xml) as an editable tree, and back to bytes. Used to adapt a
// game's manifest on the device before the system installs it.
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace venture::android::axml {
inline constexpr std::string_view kAndroidNamespace = "http://schemas.android.com/apk/res/android";
// Res_value data types used by manifests.
inline constexpr std::uint8_t kNull = 0x00, kReference = 0x01, kString = 0x03, kIntDec = 0x10, kIntHex = 0x11,
                              kBoolean = 0x12;

struct Attribute {
  std::string ns, name;            // namespace URI (empty for none) and local name
  std::uint32_t resource_id = 0;   // android.R.attr ID through the resource map; 0 for none
  std::optional<std::string> raw;  // the original text, when the writer kept it
  std::uint8_t type = kString;
  std::uint32_t data = 0;          // the typed value; ignored for strings
  std::string string;              // the value of a string attribute
};
struct Namespace {
  std::string prefix, uri;
};
struct Node {
  bool text = false;  // a CDATA node: only `value` is used
  std::string value;
  std::string ns, name;
  std::vector<Namespace> namespaces;  // declared just before this element
  std::vector<Attribute> attributes;
  std::vector<Node> children;
  std::uint32_t line = 1;

  const Attribute* attribute(std::uint32_t resource_id, std::string_view name) const;
  // Sets an android: attribute, keeping attributes in the order Android's
  // parser requires (resource IDs ascending, then attributes without one).
  void set(std::uint32_t resource_id, std::string_view name, std::string value);
  // The same for a typed value (kBoolean, kIntDec, ...).
  void set_value(std::uint32_t resource_id, std::string_view name, std::uint8_t type, std::uint32_t data);
};
struct Document {
  Node root;
};

// Throws for malformed input, styled strings, or chunks a manifest never has.
Document parse(std::span<const std::uint8_t> xml);
std::vector<std::uint8_t> serialize(const Document& document);

// A string attribute's value, or a non-string attribute's raw text.
std::string text(const Attribute& attribute);
}  // namespace venture::android::axml
