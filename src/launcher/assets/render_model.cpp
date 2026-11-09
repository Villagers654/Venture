// SteamVR render models (OBJ + MTL + PNG, described by <name>.json) found in the
// user's installation, and Venture's bundled glTF binary models, drawn by a small
// software rasterizer from any side. Nothing here writes next to the models.
#include "launcher/assets/render_model.h"
#include "common/json.h"
#include "common/steam_library.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <numbers>
#include <set>
#include <sstream>
#include <stb_image.h>

namespace venture::launcher::art {
namespace {

std::string read_file(const fs::path& path, size_t limit) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  std::string data;
  char chunk[65536];
  while (in.read(chunk, sizeof chunk) || in.gcount() > 0) {
    data.append(chunk, size_t(in.gcount()));
    if (data.size() > limit) return {};
  }
  return data;
}

// ---- Where the models are -------------------------------------------------
// OpenVR's runtime registry: per user, and on Linux also inside Flathub Steam's sandbox.
std::vector<fs::path> openvr_paths_files() {
  std::vector<fs::path> out;
#ifdef _WIN32
  if (const char* local = std::getenv("LOCALAPPDATA")) out.push_back(paths::from_utf8(local) / "openvr/openvrpaths.vrpath");
#else
  if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && *config)
    out.push_back(paths::from_utf8(config) / "openvr/openvrpaths.vrpath");
  if (const char* home = std::getenv("HOME"); home && *home) {
    out.push_back(paths::from_utf8(home) / ".config/openvr/openvrpaths.vrpath");
    out.push_back(paths::from_utf8(home) / ".var/app/com.valvesoftware.Steam/.config/openvr/openvrpaths.vrpath");
  }
#endif
  return out;
}

std::vector<fs::path> json_paths(const Json& j, std::string_view key) {
  std::vector<fs::path> out;
  if (const auto& list = field(j, key); list.is_array())
    for (const auto& entry : list)
      if (entry.is_string()) out.push_back(paths::from_utf8(entry.get<std::string>()));
  return out;
}

// Steam library roots: each Steam root and every "path" value in its
// libraryfolders.vdf (text KeyValues).
std::vector<fs::path> steam_libraries(const std::vector<fs::path>& steam_roots) {
  std::vector<fs::path> out;
  for (const fs::path& root : steam_roots) {
    if (root.empty()) continue;
    out.push_back(root);
    const std::string vdf = read_file(root / "steamapps/libraryfolders.vdf", 1 << 20);
    for (size_t at = 0; (at = vdf.find("\"path\"", at)) != std::string::npos;) {
      at += 6;
      const size_t open = vdf.find('"', at);
      if (open == std::string::npos) break;
      std::string value;
      size_t i = open + 1;
      for (; i < vdf.size() && vdf[i] != '"'; ++i) {
        if (vdf[i] == '\\' && i + 1 < vdf.size()) ++i;  // "C:\\Steam" escapes its backslashes
        value += vdf[i];
      }
      at = i;
      if (!value.empty()) out.push_back(paths::from_utf8(value));
    }
  }
  std::vector<fs::path> unique;
  for (auto& p : out) {
    std::error_code ec;
    auto canonical = fs::weakly_canonical(p, ec);
    if (ec) canonical = p;
    if (std::ranges::find(unique, canonical) == unique.end()) unique.push_back(canonical);
  }
  return unique;
}

bool named(const fs::path& path, std::string_view name) {
  return std::ranges::equal(paths::utf8(path.filename()), name,
                            [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); });
}

// Directories that hold render models (each with <model>/<model>.json).
std::vector<fs::path> render_model_roots() {
  std::vector<fs::path> roots;
  // Steam also as SteamVR's registry places it (its runtime in
  // <library>/steamapps/common, its config folder in the Steam root): the
  // client's own registration is skipped for data-dir isolated launchers
  // (VENTURE_DATA_DIR) and missing for Steam builds that never registered.
  std::vector<fs::path> steam_roots{steam_library::discover_root()};
  std::error_code ec;
  auto add_driver = [&](const fs::path& driver) {
    if (fs::is_directory(driver / "resources/rendermodels", ec)) roots.push_back(driver / "resources/rendermodels");
  };
  for (const fs::path& file : openvr_paths_files()) {
    const std::string text = read_file(file, 1 << 16);
    const Json j = Json::parse(text, nullptr, false);
    if (!j.is_discarded()) {
      for (const auto& runtime : json_paths(j, "runtime")) {
        add_driver(runtime);
        for (const auto& entry : fs::directory_iterator(runtime / "drivers", ec)) add_driver(entry.path());
        const fs::path common = runtime.parent_path();
        if (named(common, "common") && named(common.parent_path(), "steamapps"))
          steam_roots.push_back(common.parent_path().parent_path());
      }
      for (const auto& config : json_paths(j, "config"))
        if (fs::is_directory(config.parent_path() / "steamapps", ec)) steam_roots.push_back(config.parent_path());
      for (const auto& driver : json_paths(j, "external_drivers")) add_driver(driver);
    }
  }
  // Drivers shipped as Steam apps (PlayStation VR2 App/SteamVR_Plug-In): a driver
  // manifest at most two levels into an app's folder.
  for (const auto& library : steam_libraries(steam_roots)) {
    for (const auto& app : fs::directory_iterator(library / "steamapps/common", ec)) {
      if (!app.is_directory(ec)) continue;
      if (fs::exists(app.path() / "driver.vrdrivermanifest", ec)) add_driver(app.path());
      for (const auto& sub : fs::directory_iterator(app.path(), ec))
        if (sub.is_directory(ec) && fs::exists(sub.path() / "driver.vrdrivermanifest", ec)) add_driver(sub.path());
    }
  }
  std::vector<fs::path> unique;
  for (auto& root : roots)
    if (std::ranges::find(unique, root) == unique.end()) unique.push_back(std::move(root));
  return unique;
}

// ---- Geometry -------------------------------------------------------------
struct V3 {
  float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 normalize(V3 a) {
  const float n = std::sqrt(dot(a, a));
  return n > 0 ? a * (1 / n) : V3{0, 0, 1};
}

struct Texture {
  int width = 0, height = 0;
  std::vector<unsigned char> rgb;
};
struct Material {
  V3 kd{0.6f, 0.6f, 0.6f};
  const Texture* texture = nullptr;
  bool gltf = false;  // glTF: the texture is multiplied by kd and its rows run top down
};
struct Tri {
  int v[3], t[3], n[3];
  int material;
};
struct Mesh {
  std::vector<V3> v, n;
  std::vector<V3> col;  // glTF vertex colors (linear), by vertex; empty when none
  std::vector<std::array<float, 2>> t;
  std::vector<Tri> tris;
  std::vector<Material> materials;
  std::map<fs::path, std::unique_ptr<Texture>> textures;
};

float number(std::string_view& s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  float value = 0;
  auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
  s.remove_prefix(size_t(end - s.data()));
  return value;
}
std::string_view word(std::string_view& s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  size_t n = 0;
  while (n < s.size() && s[n] != ' ' && s[n] != '\t' && s[n] != '\r') ++n;
  auto w = s.substr(0, n);
  s.remove_prefix(n);
  return w;
}
std::string_view rest(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) s.remove_suffix(1);
  return s;
}

const Texture* load_texture(Mesh& mesh, const fs::path& path) {
  auto& slot = mesh.textures[path];
  if (!slot) {
    slot = std::make_unique<Texture>();
    const std::string bytes = read_file(path, 64u << 20);
    int w = 0, h = 0, n = 0;
    if (unsigned char* pixels = bytes.empty() ? nullptr
                                              : stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()),
                                                                      int(bytes.size()), &w, &h, &n, 3)) {
      slot->width = w;
      slot->height = h;
      slot->rgb.assign(pixels, pixels + size_t(w) * h * 3);
      stbi_image_free(pixels);
    }
  }
  return slot->rgb.empty() ? nullptr : slot.get();
}

// Materials by name from an MTL file.
std::map<std::string, Material> load_mtl(Mesh& mesh, const fs::path& path) {
  std::map<std::string, Material> out;
  const std::string text = read_file(path, 1 << 20);
  Material* current = nullptr;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    std::string_view s = line;
    const auto key = word(s);
    if (key == "newmtl") current = &out[std::string(rest(s))];
    else if (!current) continue;
    else if (key == "Kd") current->kd = {number(s), number(s), number(s)};
    else if (key == "map_Kd") current->texture = load_texture(mesh, path.parent_path() / paths::from_utf8(rest(s)));
  }
  return out;
}

// Appends an OBJ file; returns the range of vertices it added.
std::pair<size_t, size_t> load_obj(Mesh& mesh, const fs::path& path) {
  const size_t v0 = mesh.v.size(), t0 = mesh.t.size(), n0 = mesh.n.size();
  const std::string text = read_file(path, 256u << 20);
  std::map<std::string, Material> materials;
  int material = -1;
  auto use = [&](const Material& m) {
    mesh.materials.push_back(m);
    material = int(mesh.materials.size()) - 1;
  };
  use(Material{});
  auto index = [](std::string_view token, size_t base, size_t count) {
    int i = 0;
    std::from_chars(token.data(), token.data() + token.size(), i);
    if (i < 0) return int(count) + i;
    return i > 0 ? int(base) + i - 1 : -1;
  };
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    std::string_view s(text.data() + at, end - at);
    at = end + 1;
    const auto key = word(s);
    if (key == "v") mesh.v.push_back({number(s), number(s), number(s)});
    else if (key == "vt") mesh.t.push_back({number(s), number(s)});
    else if (key == "vn") mesh.n.push_back({number(s), number(s), number(s)});
    else if (key == "mtllib") materials = load_mtl(mesh, path.parent_path() / paths::from_utf8(rest(s)));
    else if (key == "usemtl") {
      auto it = materials.find(std::string(rest(s)));
      use(it == materials.end() ? Material{} : it->second);
    } else if (key == "f") {
      int vs[3], ts[3], ns[3], count = 0;
      for (auto token = word(s); !token.empty(); token = word(s)) {
        auto a = token.substr(0, token.find('/'));
        std::string_view b, c;
        if (auto p = token.find('/'); p != token.npos) {
          auto tail = token.substr(p + 1);
          auto q = tail.find('/');
          b = tail.substr(0, q);
          if (q != tail.npos) c = tail.substr(q + 1);
        }
        int vi = index(a, v0, mesh.v.size()), ti = b.empty() ? -1 : index(b, t0, mesh.t.size()),
            ni = c.empty() ? -1 : index(c, n0, mesh.n.size());
        if (vi < 0 || vi >= int(mesh.v.size())) break;
        if (count < 3) {
          vs[count] = vi, ts[count] = ti, ns[count] = ni;
          ++count;
        } else {  // a polygon: fan from its first corner
          vs[1] = vs[2], ts[1] = ts[2], ns[1] = ns[2];
          vs[2] = vi, ts[2] = ti, ns[2] = ni;
        }
        if (count >= 3) {
          mesh.tris.push_back({{vs[0], vs[1], vs[2]}, {ts[0], ts[1], ts[2]}, {ns[0], ns[1], ns[2]}, material});
          count = 4;
        }
      }
    }
  }
  return {v0, mesh.v.size()};
}

// SteamVR names its inputs per driver; the Controls page uses the OpenXR
// component names ("/input/squeeze" -> "squeeze").
std::string_view openxr_name(std::string_view name) {
  static constexpr std::pair<std::string_view, std::string_view> kAliases[]{
      {"grip", "squeeze"},         {"l1", "squeeze"},        {"r1", "squeeze"},       {"l2", "trigger"},
      {"r2", "trigger"},           {"left_stick", "thumbstick"}, {"right_stick", "thumbstick"},
      {"joystick", "thumbstick"},  {"bumper", "shoulder"},   {"create", "share_mndx"}, {"share", "share_mndx"},
      {"options", "options_mndx"}, {"square", "square_mndx"}, {"triangle", "triangle_mndx"},
      {"cross", "cross_mndx"},     {"circle", "circle_mndx"}, {"left_ps", "system"},    {"right_ps", "system"}, {"ps", "system"}, {"option", "options_mndx"}, {"grip_button", "squeeze"},
      {"menu_button", "menu"}, {"view_button", "view"}, {"system_button", "system"},
      {"button_grip", "squeeze"},  {"application_menu", "menu"}, {"gripsqueeze", "squeeze"},  {"button_system", "system"}, {"button_enter", "system"},
  };
  for (auto [from, to] : kAliases)
    if (name == from) return to;
  if (name.starts_with("button_")) name.remove_prefix(7);
  return name;
}
std::string input_name(std::string_view path) {
  if (!path.starts_with("/input/")) return {};
  path.remove_prefix(7);
  return std::string(openxr_name(path.substr(0, path.find('/'))));
}

}  // namespace

std::optional<fs::path> find_render_model(std::string_view name) {
  if (name.empty() || name.find_first_of("/\\.") != name.npos) return std::nullopt;
  std::error_code ec;
  for (const auto& root : render_model_roots()) {
    const fs::path dir = root / paths::from_utf8(name);
    if (fs::is_regular_file(dir / (paths::from_utf8(name).native() + fs::path(".json").native()), ec)) return dir;
  }
  return std::nullopt;
}

namespace {
// The inputs a group of vertices shows (a button, the stick...).
struct Part {
  std::vector<std::string> inputs;
  std::vector<std::pair<size_t, size_t>> ranges;  // vertex ranges
  V3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
  V3 center() const { return (lo + hi) * 0.5f; }
};
struct Model {
  Mesh mesh;
  std::vector<Part> parts;
  std::vector<int> part_of;  // the input part each vertex belongs to, or -1
  // Light closer to the bundled pictures' renderer (brighter, with a soft
  // reflection of the room), so turning one does not change its look much.
  float gain = 1, sheen = 0, gloss = 1, curve = 1;
  V3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
  // Fills part_of, the parts' boxes and the model's box.
  void finish() {
    part_of.assign(mesh.v.size(), -1);
    for (size_t p = 0; p < parts.size(); ++p)
      for (auto [a, b] : parts[p].ranges)
        for (size_t i = a; i < b; ++i) {
          part_of[i] = int(p);
          const V3 v = mesh.v[i];
          parts[p].lo = {std::min(parts[p].lo.x, v.x), std::min(parts[p].lo.y, v.y), std::min(parts[p].lo.z, v.z)};
          parts[p].hi = {std::max(parts[p].hi.x, v.x), std::max(parts[p].hi.y, v.y), std::max(parts[p].hi.z, v.z)};
        }
    for (const auto& tri : mesh.tris)
      for (int k : tri.v) {
        const V3 v = mesh.v[k];
        lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
        hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
      }
  }
};

std::optional<Model> load_model(const fs::path& dir) {
  const std::string name = paths::utf8(dir.filename());
  const Json j = Json::parse(read_file(dir / (name + ".json"), 1 << 20), nullptr, false);
  if (j.is_discarded()) return std::nullopt;
  Model model;
  for (const auto& [component, c] : field(j, "components").items()) {
    const std::string file = str(c, "filename");
    if (file.empty() || file.find_first_of("/\\") != file.npos) continue;
    if (const auto& vis = field(c, "visibility"); vis.is_object() && vis.value("default", true) == false) continue;
    auto [v0, v1] = load_obj(model.mesh, dir / paths::from_utf8(file));
    Part part{{}, {{v0, v1}}};
    // The inputs it shows: its paths in the model description, its name, and
    // its file's name, which some drivers keep truer than the path (Quest's menu
    // button is "button_menu.obj" but reports /input/system).
    const auto& motion = field(c, "motion");
    for (const char* key : {"pressed_path", "trigger_path", "component_path", "joystick_path", "touch_path"})
      if (auto n = input_name(str(motion, key)); !n.empty()) part.inputs.push_back(n);
    if (part.inputs.empty() && field(motion, "dpad_up_path").is_string())
      part.inputs = {"dpad_up", "dpad_down", "dpad_left", "dpad_right"};
    if (!component.starts_with("body")) part.inputs.push_back(std::string(openxr_name(component)));
    if (std::string stem = paths::utf8(fs::path(paths::from_utf8(file)).stem()); stem.starts_with(name + "_")) {
      stem.erase(0, name.size() + 1);
      if (!stem.starts_with("body")) part.inputs.push_back(std::string(openxr_name(stem)));
    }
    if (!part.inputs.empty()) model.parts.push_back(std::move(part));
  }
  if (model.mesh.tris.empty()) return std::nullopt;
  model.finish();
  return model;
}

// ---- glTF binary ----------------------------------------------------------
// The input each WebXR Input Profiles node shows (tools/controller_art's NODES).
std::string_view webxr_input(std::string_view node) {
  static constexpr std::pair<std::string_view, std::string_view> kNodes[]{
      {"a_button", "a"}, {"A", "a"}, {"b_button", "b"}, {"B", "b"}, {"x_button", "x"}, {"X", "x"},
      {"y_button", "y"}, {"Y", "y"}, {"x_button_mesh", "x"}, {"y_button_mesh", "y"}, {"a_button_mesh", "a"},
      {"b_button_mesh", "b"}, {"thumbstick", "thumbstick"}, {"joystick", "thumbstick"},
      {"thumbstick_mesh", "thumbstick"}, {"THUMBSTICK_PRESS", "thumbstick"}, {"touchpad", "trackpad"},
      {"trigger", "trigger"}, {"trigger_mesh", "trigger"}, {"SELECT", "trigger"}, {"squeeze", "squeeze"},
      {"grip", "squeeze"}, {"left_squeeze", "squeeze"}, {"right_squeeze", "squeeze"}, {"squeeze_mesh", "squeeze"},
      {"GRASP", "squeeze"}, {"menu_button", "menu"}, {"menu_mesh", "menu"}, {"MENU", "menu"},
      {"bumper", "bumper"}, {"HOME", "system"},
  };
  for (auto [from, to] : kNodes)
    if (node == from) return to;
  return {};
}

struct Mat4 {
  float m[16]{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // column major, as glTF
  V3 point(V3 p) const {
    return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
            m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
  }
  V3 vector(V3 p) const {
    return {m[0] * p.x + m[4] * p.y + m[8] * p.z, m[1] * p.x + m[5] * p.y + m[9] * p.z,
            m[2] * p.x + m[6] * p.y + m[10] * p.z};
  }
  Mat4 operator*(const Mat4& b) const {
    Mat4 r;
    for (int c = 0; c < 4; ++c)
      for (int row = 0; row < 4; ++row) {
        float sum = 0;
        for (int k = 0; k < 4; ++k) sum += m[k * 4 + row] * b.m[c * 4 + k];
        r.m[c * 4 + row] = sum;
      }
    return r;
  }
};

Mat4 node_matrix(const Json& node) {
  Mat4 out;
  if (const auto& mat = field(node, "matrix"); mat.is_array() && mat.size() == 16) {
    for (int i = 0; i < 16; ++i) out.m[i] = mat[size_t(i)].get<float>();
    return out;
  }
  float t[3]{0, 0, 0}, q[4]{0, 0, 0, 1}, sc[3]{1, 1, 1};
  auto read = [&](std::string_view key, float* to, size_t n) {
    if (const auto& a = field(node, key); a.is_array() && a.size() == n)
      for (size_t i = 0; i < n; ++i) to[i] = a[i].get<float>();
  };
  read("translation", t, 3), read("rotation", q, 4), read("scale", sc, 3);
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  const float r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w),     2 * (x * z - y * w),
                      2 * (x * y - z * w),     1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
                      2 * (x * z + y * w),     2 * (y * z - x * w),     1 - 2 * (x * x + y * y)};  // columns
  for (int c = 0; c < 3; ++c)
    for (int row = 0; row < 3; ++row) out.m[c * 4 + row] = r[c * 3 + row] * sc[c];
  out.m[12] = t[0], out.m[13] = t[1], out.m[14] = t[2];
  return out;
}

struct Glb {
  Json doc;
  std::span<const unsigned char> bin;
  // Accessor `index` as floats, `width` per element (VEC3 = 3); empty when unusable.
  std::vector<float> floats(int index, int width) const {
    const auto& accessors = field(doc, "accessors");
    if (index < 0 || size_t(index) >= accessors.size()) return {};
    const Json& a = accessors[size_t(index)];
    const auto& views = field(doc, "bufferViews");
    const int vi = a.value("bufferView", -1);
    if (vi < 0 || size_t(vi) >= views.size()) return {};
    const Json& v = views[size_t(vi)];
    const int type = a.value("componentType", 0);
    const size_t count = a.value("count", size_t(0));
    const std::string kind = a.value("type", std::string());
    const int comps = kind == "SCALAR" ? 1 : kind == "VEC2" ? 2 : kind == "VEC3" ? 3 : kind == "VEC4" ? 4 : 0;
    const size_t size = type == 5126 || type == 5125 ? 4 : type == 5123 || type == 5122 ? 2 : 1;
    if (!comps || width > comps) return {};
    const size_t stride = v.value("byteStride", size_t(0)) ? v.value("byteStride", size_t(0)) : size * comps;
    const size_t start = v.value("byteOffset", size_t(0)) + a.value("byteOffset", size_t(0));
    if (count && start + stride * (count - 1) + size * comps > bin.size()) return {};
    const bool normalized = a.value("normalized", false);
    std::vector<float> out(count * size_t(width));
    for (size_t i = 0; i < count; ++i)
      for (int c = 0; c < width; ++c) {
        const unsigned char* p = bin.data() + start + stride * i + size * size_t(c);
        float value = 0;
        if (type == 5126) std::memcpy(&value, p, 4);
        else if (type == 5125) { uint32_t u; std::memcpy(&u, p, 4); value = float(u); }
        else if (type == 5123) { uint16_t u; std::memcpy(&u, p, 2); value = normalized ? float(u) / 65535.f : float(u); }
        else if (type == 5121) value = normalized ? float(*p) / 255.f : float(*p);
        else return {};
        out[i * size_t(width) + size_t(c)] = value;
      }
    return out;
  }
};

std::optional<Glb> parse_glb(std::span<const unsigned char> data) {
  auto u32 = [&](size_t at) {
    uint32_t v = 0;
    std::memcpy(&v, data.data() + at, 4);
    return v;
  };
  if (data.size() < 20 || u32(0) != 0x46546C67 || u32(4) != 2) return std::nullopt;
  const size_t json_len = u32(12);
  if (u32(16) != 0x4E4F534A || 20 + json_len > data.size()) return std::nullopt;
  Glb glb;
  glb.doc = Json::parse(data.begin() + 20, data.begin() + 20 + std::ptrdiff_t(json_len), nullptr, false);
  if (glb.doc.is_discarded()) return std::nullopt;
  const size_t bin_at = 20 + json_len;
  if (bin_at + 8 <= data.size() && u32(bin_at + 4) == 0x004E4942)
    glb.bin = data.subspan(bin_at + 8, std::min<size_t>(u32(bin_at), data.size() - bin_at - 8));
  return glb;
}

// The model in a glTF binary; `nodes` gets each node's vertex range (its own
// meshes and its children's) by node index.
std::optional<Model> load_glb(std::span<const unsigned char> data,
                              std::vector<std::pair<size_t, size_t>>* nodes) {
  auto glb = parse_glb(data);
  if (!glb) return std::nullopt;
  const Json& doc = glb->doc;
  Model model;
  Mesh& mesh = model.mesh;
  // Materials: base color only.
  std::vector<int> material_of;
  for (const auto& m : field(doc, "materials")) {
    Material mat;
    mat.gltf = true;
    mat.kd = {1, 1, 1};
    const auto& pbr = field(m, "pbrMetallicRoughness");
    if (const auto& f = field(pbr, "baseColorFactor"); f.is_array() && f.size() >= 3)
      mat.kd = {f[0].get<float>(), f[1].get<float>(), f[2].get<float>()};
    if (const auto& t = field(pbr, "baseColorTexture"); t.is_object()) {
      const int ti = t.value("index", -1);
      const auto& textures = field(doc, "textures");
      const int ii = ti >= 0 && size_t(ti) < textures.size() ? textures[size_t(ti)].value("source", -1) : -1;
      const auto& images = field(doc, "images");
      const int vi = ii >= 0 && size_t(ii) < images.size() ? images[size_t(ii)].value("bufferView", -1) : -1;
      const auto& views = field(doc, "bufferViews");
      if (vi >= 0 && size_t(vi) < views.size()) {
        const size_t at = views[size_t(vi)].value("byteOffset", size_t(0)),
                     len = views[size_t(vi)].value("byteLength", size_t(0));
        auto& slot = mesh.textures[fs::path(std::format("glb-image-{}", ii))];
        if (!slot && at + len <= glb->bin.size()) {
          slot = std::make_unique<Texture>();
          int w = 0, h = 0, n = 0;
          if (unsigned char* px = stbi_load_from_memory(glb->bin.data() + at, int(len), &w, &h, &n, 3)) {
            slot->width = w, slot->height = h;
            slot->rgb.assign(px, px + size_t(w) * h * 3);
            stbi_image_free(px);
          }
        }
        if (slot && !slot->rgb.empty()) mat.texture = slot.get();
      }
    }
    mesh.materials.push_back(mat);
  }
  Material plain;
  plain.gltf = true;
  plain.kd = {0.8f, 0.8f, 0.8f};
  mesh.materials.push_back(plain);
  const int plain_index = int(mesh.materials.size()) - 1;

  const auto& all_nodes = field(doc, "nodes");
  const auto& meshes = field(doc, "meshes");
  if (nodes) nodes->assign(all_nodes.size(), {0, 0});
  std::vector<bool> visited(all_nodes.size());
  auto walk = [&](auto&& self, int index, const Mat4& parent) -> void {
    if (index < 0 || size_t(index) >= all_nodes.size() || visited[size_t(index)]) return;
    visited[size_t(index)] = true;
    const Json& node = all_nodes[size_t(index)];
    std::string upper = node.value("name", std::string());
    std::ranges::transform(upper, upper.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    // A tracking helper mesh is not the controller's shell; its children are
    // (render_controller.py skips the same objects).
    const bool helper = upper.find("TRACKING") != upper.npos;
    const Mat4 world = parent * node_matrix(node);
    const size_t first = mesh.v.size();
    if (const int mi = node.value("mesh", -1); !helper && mi >= 0 && size_t(mi) < meshes.size()) {
      for (const auto& prim : field(meshes[size_t(mi)], "primitives")) {
        if (prim.value("mode", 4) != 4) continue;
        const auto& attrs = field(prim, "attributes");
        const auto pos = glb->floats(attrs.value("POSITION", -1), 3);
        if (pos.empty()) continue;
        const size_t count = pos.size() / 3, base = mesh.v.size();
        const auto nrm = glb->floats(attrs.value("NORMAL", -1), 3);
        const auto uv = glb->floats(attrs.value("TEXCOORD_0", -1), 2);
        const auto col = glb->floats(attrs.value("COLOR_0", -1), 3);
        for (size_t i = 0; i < count; ++i) {
          mesh.v.push_back(world.point({pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2]}));
          mesh.n.push_back(nrm.size() == count * 3 ? normalize(world.vector({nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]}))
                                                   : V3{0, 0, 0});
          mesh.t.push_back(uv.size() == count * 2 ? std::array<float, 2>{uv[i * 2], uv[i * 2 + 1]}
                                                  : std::array<float, 2>{0, 0});
        }
        if (!col.empty() || !mesh.col.empty()) {
          mesh.col.resize(base, V3{1, 1, 1});
          for (size_t i = 0; i < count; ++i)
            mesh.col.push_back(col.size() == count * 3 ? V3{col[i * 3], col[i * 3 + 1], col[i * 3 + 2]} : V3{1, 1, 1});
        }
        int material = prim.value("material", -1);
        if (material < 0 || material >= plain_index) material = plain_index;
        const bool has_n = nrm.size() == count * 3, has_t = uv.size() == count * 2;
        auto add = [&](size_t a, size_t b, size_t c) {
          if (a >= count || b >= count || c >= count) return;
          const int ia = int(base + a), ib = int(base + b), ic = int(base + c);
          mesh.tris.push_back({{ia, ib, ic},
                               {has_t ? ia : -1, has_t ? ib : -1, has_t ? ic : -1},
                               {has_n ? ia : -1, has_n ? ib : -1, has_n ? ic : -1},
                               material});
        };
        if (prim.contains("indices")) {
          const auto idx = glb->floats(prim.value("indices", -1), 1);
          for (size_t i = 0; i + 2 < idx.size(); i += 3) add(size_t(idx[i]), size_t(idx[i + 1]), size_t(idx[i + 2]));
        } else {
          for (size_t i = 0; i + 2 < count; i += 3) add(i, i + 1, i + 2);
        }
      }
    }
    for (const auto& child : field(node, "children")) self(self, child.get<int>(), world);
    if (nodes) (*nodes)[size_t(index)] = {first, mesh.v.size()};
  };
  const auto& scenes = field(doc, "scenes");
  const size_t scene = doc.value("scene", size_t(0));
  if (scene < scenes.size())
    for (const auto& root : field(scenes[scene], "nodes")) walk(walk, root.get<int>(), Mat4{});
  if (!mesh.col.empty()) mesh.col.resize(mesh.v.size(), V3{1, 1, 1});
  if (mesh.tris.empty()) return std::nullopt;
  return model;
}

constexpr float kDeg = std::numbers::pi_v<float> / 180;
struct Camera {
  V3 d, r, u;  // toward the camera, picture right, picture up
};
// The same view of the other hand: mirrored across X, and flipped left-right so
// the pair reads as mirror images.
V3 mirror(V3 v) { return {-v.x, v.y, v.z}; }
Camera mirrored(const Camera& c) { return {mirror(c.d), mirror(c.r) * -1, mirror(c.u)}; }

// The picture area: x0..x1 and y0..y1 in camera units.
struct Frame {
  float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
};
Frame extent(const Mesh& mesh, const Camera& cam) {
  Frame f;
  for (const auto& tri : mesh.tris)
    for (int k : tri.v) {
      const float x = dot(mesh.v[k], cam.r), y = dot(mesh.v[k], cam.u);
      f.x0 = std::min(f.x0, x), f.x1 = std::max(f.x1, x), f.y0 = std::min(f.y0, y), f.y1 = std::max(f.y1, y);
    }
  return f;
}

// Stands the controller up: its long axis (the handle) up the picture, head end
// on top (OpenVR controllers hold the head up and forward, +Y and -Z), seen from
// `az` around that axis (0 = its +X side) and `el` above.
struct Upright {
  V3 axis{0, 1, 0};
  explicit Upright(const Mesh& mesh) {
    V3 mean{};
    for (const V3& p : mesh.v) mean = mean + p;
    mean = mean * (1.f / float(mesh.v.size()));
    double c[3][3]{};
    for (const V3& p : mesh.v) {
      const V3 q = p - mean;
      const float e[3] = {q.x, q.y, q.z};
      for (int i = 0; i < 3; ++i)
        for (int k = 0; k < 3; ++k) c[i][k] += double(e[i]) * e[k];
    }
    V3 v{0.3f, 1, -0.2f};  // power iteration for the main axis
    for (int it = 0; it < 64; ++it)
      v = normalize({float(c[0][0] * v.x + c[0][1] * v.y + c[0][2] * v.z),
                     float(c[1][0] * v.x + c[1][1] * v.y + c[1][2] * v.z),
                     float(c[2][0] * v.x + c[2][1] * v.y + c[2][2] * v.z)});
    axis = dot(v, {0, 1, -1}) >= 0 ? v : v * -1;
  }
  Camera at(float az, float el) const {
    const V3 side = normalize(V3{1, 0, 0} - axis * axis.x), front = cross(axis, side);
    Camera c;
    c.d = normalize((side * std::cos(az * kDeg) + front * std::sin(az * kDeg)) * std::cos(el * kDeg) +
                    axis * std::sin(el * kDeg));
    c.u = normalize(axis - c.d * dot(axis, c.d));
    c.r = normalize(cross(c.d * -1, c.u));
    return c;
  }
};

Camera fixed_camera(const View& view) {
  Camera c;
  c.d = normalize({std::sin(view.azimuth * kDeg) * std::cos(view.elevation * kDeg), std::sin(view.elevation * kDeg),
                   std::cos(view.azimuth * kDeg) * std::cos(view.elevation * kDeg)});
  const V3 r = normalize(cross(c.d * -1, {0, 1, 0})), u = cross(r, c.d * -1);
  c.r = r * std::cos(view.roll * kDeg) + u * std::sin(view.roll * kDeg);
  c.u = u * std::cos(view.roll * kDeg) - r * std::sin(view.roll * kDeg);
  return c;
}

const std::array<float, 256>& to_linear() {
  static const auto lut = [] {
    std::array<float, 256> l{};
    for (int i = 0; i < 256; ++i) l[i] = std::pow(float(i) / 255.f, 2.2f);
    return l;
  }();
  return lut;
}

struct Raster {
  int W = 0, H = 0;  // samples
  std::vector<float> depth;
  std::vector<std::array<float, 3>> color;
  std::vector<int> owner;  // the input part each sample shows, or -1
};
// Draws `model` with its vertices at `screen` (sample x, y and depth toward the
// viewer) into a W x H sample grid, lit as seen through `cam`.
Raster raster(const Model& model, const Camera& cam, const std::vector<V3>& screen, int W, int H, bool shade,
              bool owners) {
  const Mesh& mesh = model.mesh;
  Raster f;
  f.W = W, f.H = H;
  if (W <= 0 || H <= 0) return f;
  f.depth.assign(size_t(W) * H, -std::numeric_limits<float>::infinity());
  if (owners) f.owner.assign(size_t(W) * H, -1);
  if (shade) f.color.resize(size_t(W) * H);
  // Lights in the camera frame: key from the upper left, a soft fill, a rim from behind.
  const V3 key = normalize(cam.r * -0.45f + cam.u * 0.65f + cam.d * 0.65f),
           fill = normalize(cam.r * 0.7f + cam.u * -0.1f + cam.d * 0.6f), rim = normalize(cam.u * 0.6f + cam.d * -0.8f),
           half = normalize(key + cam.d);
  const auto& lin = to_linear();
  const bool colors = !mesh.col.empty();
  for (const auto& tri : mesh.tris) {
    const V3 a = screen[tri.v[0]], b = screen[tri.v[1]], c = screen[tri.v[2]];
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::abs(area) < 1e-6f) continue;
    const int px0 = std::max(0, int(std::floor(std::min({a.x, b.x, c.x})))),
              px1 = std::min(W - 1, int(std::ceil(std::max({a.x, b.x, c.x})))),
              py0 = std::max(0, int(std::floor(std::min({a.y, b.y, c.y})))),
              py1 = std::min(H - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
    if (px0 > px1 || py0 > py1) continue;
    const V3 face = normalize(cross(mesh.v[tri.v[1]] - mesh.v[tri.v[0]], mesh.v[tri.v[2]] - mesh.v[tri.v[0]]));
    const Material& m = mesh.materials[tri.material];
    const bool has_n = tri.n[0] >= 0 && tri.n[1] >= 0 && tri.n[2] >= 0;
    const bool has_t = m.texture && tri.t[0] >= 0 && tri.t[1] >= 0 && tri.t[2] >= 0;
    const int part = owners ? model.part_of[tri.v[0]] : -1;
    const float inv = 1 / area;
    for (int py = py0; py <= py1; ++py)
      for (int px = px0; px <= px1; ++px) {
        const float x = float(px) + 0.5f, y = float(py) + 0.5f;
        const float w0 = ((b.x - x) * (c.y - y) - (b.y - y) * (c.x - x)) * inv,
                    w1 = ((c.x - x) * (a.y - y) - (c.y - y) * (a.x - x)) * inv, w2 = 1 - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        const float z = a.z * w0 + b.z * w1 + c.z * w2;
        const size_t at = size_t(py) * W + px;
        if (z <= f.depth[at]) continue;
        f.depth[at] = z;
        if (owners) f.owner[at] = part;
        if (!shade) continue;
        V3 n = has_n ? normalize(mesh.n[tri.n[0]] * w0 + mesh.n[tri.n[1]] * w1 + mesh.n[tri.n[2]] * w2) : face;
        if (has_n && dot(n, n) < 0.5f) n = face;
        if (dot(n, cam.d) < 0) n = n * -1;  // two-sided
        V3 albedo = m.kd;
        if (has_t) {
          const auto& t0 = mesh.t[tri.t[0]];
          const auto& t1 = mesh.t[tri.t[1]];
          const auto& t2 = mesh.t[tri.t[2]];
          float tu = t0[0] * w0 + t1[0] * w1 + t2[0] * w2, tv = t0[1] * w0 + t1[1] * w1 + t2[1] * w2;
          if (!m.gltf) tv = 1 - tv;
          tu -= std::floor(tu), tv -= std::floor(tv);
          const Texture& tx = *m.texture;
          const float fx = tu * float(tx.width) - 0.5f, fy = tv * float(tx.height) - 0.5f;
          const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
          const float ax = fx - float(ix), ay = fy - float(iy);
          float rgb[3] = {};
          for (int k = 0; k < 4; ++k) {
            const int sx = ((ix + (k & 1)) % tx.width + tx.width) % tx.width,
                      sy = ((iy + (k >> 1)) % tx.height + tx.height) % tx.height;
            const float wk = ((k & 1) ? ax : 1 - ax) * ((k >> 1) ? ay : 1 - ay);
            const unsigned char* p = &tx.rgb[(size_t(sy) * tx.width + sx) * 3];
            for (int ch = 0; ch < 3; ++ch) rgb[ch] += lin[p[ch]] * wk;
          }
          albedo = m.gltf ? V3{rgb[0] * m.kd.x, rgb[1] * m.kd.y, rgb[2] * m.kd.z} : V3{rgb[0], rgb[1], rgb[2]};
        }
        if (colors) {
          const V3 vc = mesh.col[tri.v[0]] * w0 + mesh.col[tri.v[1]] * w1 + mesh.col[tri.v[2]] * w2;
          albedo = {albedo.x * vc.x, albedo.y * vc.y, albedo.z * vc.z};
        }
        const float light = 0.42f + 0.8f * std::max(0.f, dot(n, key)) + 0.22f * std::max(0.f, dot(n, fill));
        const float spec =
            0.18f * std::pow(std::max(0.f, dot(n, half)), 36.f) + 0.12f * std::pow(std::max(0.f, dot(n, rim)), 2.f);
        const float g = light * model.gain, add = spec * model.gloss + model.sheen;
        f.color[at] = {albedo.x * g + add, albedo.y * g + add, albedo.z * g + add};
      }
  }
  return f;
}

// The vertices through `cam` into `frame`, `height` pixels tall at `ss` samples
// per pixel, as raster() takes them; `width` gets the picture width in pixels.
std::vector<V3> frame_screen(const Mesh& mesh, const Camera& cam, const Frame& frame, int height, int ss, int* width) {
  const float margin = 1.04f;
  const float scale = float(height * ss) / ((frame.y1 - frame.y0) * margin);
  *width = std::max(1, int(std::ceil((frame.x1 - frame.x0) * scale / float(ss) + float(height) * (margin - 1))));
  const float W = float(*width * ss), H = float(height * ss);
  const float cx = (frame.x0 + frame.x1) * 0.5f, cy = (frame.y0 + frame.y1) * 0.5f;
  std::vector<V3> screen(mesh.v.size());
  for (size_t i = 0; i < mesh.v.size(); ++i) {
    const V3 p = mesh.v[i];
    screen[i] = {(dot(p, cam.r) - cx) * scale + W * 0.5f, H * 0.5f - (dot(p, cam.u) - cy) * scale, dot(p, cam.d)};
  }
  return screen;
}

// The view that shows the inputs best, from the side as a hand holds it: every
// input part that shows counts most, then each part by the square root of its
// visible area (every button a little beats one button large); a wide picture
// costs a little.
Camera best_camera(const Model& model) {
  const Upright upright(model.mesh);
  Camera best_cam = upright.at(-90, 20);
  double best = -1;
  for (float el : {0.f, 20.f, 40.f})
    for (float az = -180; az < 180; az += 15) {
      const Camera c = upright.at(az, el);
      const Frame fr = extent(model.mesh, c);
      if (!(fr.y1 > fr.y0) || !(fr.x1 > fr.x0)) continue;
      int width = 0;
      const auto screen = frame_screen(model.mesh, c, fr, 160, 1, &width);
      const Raster f = raster(model, c, screen, width, 160, false, true);
      std::vector<int> seen(model.parts.size());
      for (int o : f.owner)
        if (o >= 0) ++seen[size_t(o)];
      double area = 0, shown = 0;
      for (int px : seen) area += std::sqrt(double(px)), shown += px >= 12 ? 1 : 0;
      const double score = (shown * 100 + area) / std::sqrt(std::max(1.0, double(width) / 160.0));
      if (score > best) best = score, best_cam = c;
    }
  return best_cam;
}
}  // namespace

struct Scene {
  std::shared_ptr<const Model> model;
  Camera cam;              // the unturned view
  float scale = 1;         // picture pixels per model unit
  int width = 0, height = 0;
  V3 pivot;                // turns happen about it
  float px = 0, py = 0;    // the pivot's picture position
  float slack = 0;         // depth slack for surface points, in model units
  struct Point {
    std::string input;
    int part = -1;  // shown by this part, else a point on the surface
    V3 at;
  };
  std::vector<Point> points;

  // A scene whose unturned picture puts camera point (cx, cy) in the middle.
  Scene(std::shared_ptr<const Model> m, const Camera& c, float s, int w, int h, float cx, float cy)
      : model(std::move(m)), cam(c), scale(s), width(w), height(h) {
    pivot = (model->lo + model->hi) * 0.5f;
    px = (dot(pivot, cam.r) - cx) * scale + float(width) * 0.5f;
    py = float(height) * 0.5f - (dot(pivot, cam.u) - cy) * scale;
    const V3 size = model->hi - model->lo;
    slack = 0.03f * std::sqrt(dot(size, size));
  }
};

bool Turn::identity() const { return *this == Turn{}; }

void Turn::drag(float dx, float dy) {
  const float a = dx * kDeg, b = dy * kDeg;
  const float ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b);
  // About the picture's vertical axis (near side toward +x), then its horizontal
  // axis (near side down, as the pointer moves down the screen).
  const float ry[9] = {ca, 0, sa, 0, 1, 0, -sa, 0, ca};
  const float rx[9] = {1, 0, 0, 0, cb, -sb, 0, sb, cb};
  float d[9], out[9];
  for (int i = 0; i < 3; ++i)
    for (int k = 0; k < 3; ++k) d[i * 3 + k] = rx[i * 3] * ry[k] + rx[i * 3 + 1] * ry[3 + k] + rx[i * 3 + 2] * ry[6 + k];
  for (int i = 0; i < 3; ++i)
    for (int k = 0; k < 3; ++k) out[i * 3 + k] = d[i * 3] * m[k] + d[i * 3 + 1] * m[3 + k] + d[i * 3 + 2] * m[6 + k];
  // Keep it a rotation (Gram-Schmidt on the rows).
  V3 r0{out[0], out[1], out[2]}, r1{out[3], out[4], out[5]};
  r0 = normalize(r0);
  r1 = normalize(r1 - r0 * dot(r0, r1));
  const V3 r2 = cross(r0, r1);
  m = {r0.x, r0.y, r0.z, r1.x, r1.y, r1.z, r2.x, r2.y, r2.z};
}

std::array<int, 2> scene_size(const Scene& scene) { return {scene.width, scene.height}; }

Shot draw(const Scene& scene, const Turn& turn, float scale, int ss, bool whole) {
  const Model& model = *scene.model;
  const Mesh& mesh = model.mesh;
  const auto& R = turn.m;
  const Camera& c0 = scene.cam;
  Camera cam;
  cam.r = c0.r * R[0] + c0.u * R[1] + c0.d * R[2];
  cam.u = c0.r * R[3] + c0.u * R[4] + c0.d * R[5];
  cam.d = c0.r * R[6] + c0.u * R[7] + c0.d * R[8];
  // Unturned picture pixels (x, y) and depth toward the viewer.
  std::vector<V3> pic(mesh.v.size());
  Shot shot;
  shot.turn = turn;
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  for (size_t i = 0; i < mesh.v.size(); ++i) {
    const V3 p = mesh.v[i] - scene.pivot;
    pic[i] = {scene.px + dot(p, cam.r) * scene.scale, scene.py - dot(p, cam.u) * scene.scale, dot(p, cam.d)};
  }
  for (const auto& tri : mesh.tris)
    for (int k : tri.v)
      x0 = std::min(x0, pic[k].x), x1 = std::max(x1, pic[k].x), y0 = std::min(y0, pic[k].y), y1 = std::max(y1, pic[k].y);
  if (whole) {
    x0 = 0, y0 = 0, x1 = float(scene.width), y1 = float(scene.height);
  } else {
    x0 = std::floor(x0) - 2, y0 = std::floor(y0) - 2, x1 = std::ceil(x1) + 2, y1 = std::ceil(y1) + 2;
  }
  if (!(x1 > x0) || !(y1 > y0) || !(scale > 0)) return shot;
  shot.width = std::max(1, int(std::lround((x1 - x0) * scale)));
  shot.height = std::max(1, int(std::lround((y1 - y0) * scale)));
  shot.x0 = x0, shot.y0 = y0;
  shot.x1 = x0 + float(shot.width) / scale, shot.y1 = y0 + float(shot.height) / scale;
  const float k = scale * float(ss);
  std::vector<V3> screen(pic.size());
  for (size_t i = 0; i < pic.size(); ++i) screen[i] = {(pic[i].x - x0) * k, (pic[i].y - y0) * k, pic[i].z};
  const int W = shot.width * ss, H = shot.height * ss;
  const Raster f = raster(model, cam, screen, W, H, true, true);

  shot.rgba.resize(size_t(shot.width) * shot.height * 4);
  shot.depth.assign(size_t(shot.width) * shot.height, -std::numeric_limits<float>::infinity());
  shot.slack = scene.slack;
  for (int y = 0; y < shot.height; ++y)
    for (int x = 0; x < shot.width; ++x) {
      float sum[3] = {}, cover = 0;
      float& near = shot.depth[size_t(y) * shot.width + x];
      for (int s = 0; s < ss * ss; ++s) {
        const size_t at = size_t(y * ss + s / ss) * W + size_t(x * ss + s % ss);
        if (!std::isfinite(f.depth[at])) continue;
        near = std::max(near, f.depth[at]);
        cover += 1;
        for (int ch = 0; ch < 3; ++ch) sum[ch] += f.color[at][ch];
      }
      unsigned char* p = &shot.rgba[(size_t(y) * shot.width + x) * 4];
      if (cover == 0) continue;
      for (int ch = 0; ch < 3; ++ch) {
        const float v = std::clamp(sum[ch] / cover, 0.f, 1.f);
        p[ch] = (unsigned char)std::lround(std::pow(v, model.curve / 2.2f) * 255);
      }
      p[3] = (unsigned char)std::lround(cover / float(ss * ss) * 255);
    }

  // The inputs: where they sit and whether the model is in front of them.
  std::vector<int> seen(model.parts.size());
  for (int o : f.owner)
    if (o >= 0) ++seen[size_t(o)];
  const float per_pixel = k * k;  // samples per unturned picture pixel
  for (const auto& point : scene.points) {
    const V3 at = (point.part >= 0 ? model.parts[size_t(point.part)].center() : point.at) - scene.pivot;
    Anchor a{point.input, scene.px + dot(at, cam.r) * scene.scale, scene.py - dot(at, cam.u) * scene.scale, false,
             dot(at, cam.d)};
    if (point.part >= 0) {
      a.hidden = float(seen[size_t(point.part)]) < 3 * per_pixel;
      // The dot sits at the middle of the part's bounds, which can fall behind other
      // geometry (a tracking ring) or in open space beside a curved part. Where the
      // part shows near that middle the dot moves onto the nearest shown sample of it;
      // where none of it shows nearby and something stands in front of the middle,
      // the input is hidden from this side (a Touch trigger behind the grip, its tip
      // showing far below).
      if (!a.hidden) {
        const int sx = int((a.x - x0) * k), sy = int((a.y - y0) * k);
        const int near = std::max(1, int(k)), reach = std::max(2, int(std::ceil(12 * k)));
        int best = -1, bx = sx, by = sy;
        float front = -std::numeric_limits<float>::infinity();
        for (int yy = std::max(0, sy - reach); yy <= std::min(H - 1, sy + reach); ++yy)
          for (int xx = std::max(0, sx - reach); xx <= std::min(W - 1, sx + reach); ++xx) {
            const size_t at = size_t(yy) * W + xx;
            const int d2 = (xx - sx) * (xx - sx) + (yy - sy) * (yy - sy);
            if (f.owner[at] == point.part) {
              if (d2 <= reach * reach && (best < 0 || d2 < best)) best = d2, bx = xx, by = yy;
            } else if (std::abs(xx - sx) <= near && std::abs(yy - sy) <= near) {
              front = std::max(front, f.depth[at]);
            }
          }
        if (best > near * near * 2) {
          a.x = x0 + (float(bx) + 0.5f) / k;
          a.y = y0 + (float(by) + 0.5f) / k;
          a.z = f.depth[size_t(by) * W + bx];
        } else if (best < 0) {
          a.hidden = std::isfinite(front) && front > a.z + scene.slack;
        }
      }
    } else {
      // In front of whatever covers it nearby, give or take the slack.
      const float z = a.z;
      const int sx = int((a.x - x0) * k), sy = int((a.y - y0) * k), reach = std::max(1, int(k));
      float front = -std::numeric_limits<float>::infinity();
      for (int yy = sy - reach; yy <= sy + reach; ++yy)
        for (int xx = sx - reach; xx <= sx + reach; ++xx)
          if (xx >= 0 && yy >= 0 && xx < W && yy < H) front = std::max(front, f.depth[size_t(yy) * W + xx]);
      a.hidden = std::isfinite(front) && front > z + scene.slack;
    }
    shot.anchors.push_back(std::move(a));
  }
  return shot;
}

std::vector<std::pair<float, float>> covered_spans(const Shot& shot, const Anchor& anchor, float fx, float fy,
                                                   float tx, float ty) {
  std::vector<std::pair<float, float>> spans;
  if (shot.width <= 0 || shot.depth.size() != size_t(shot.width) * shot.height || !(shot.x1 > shot.x0)) return spans;
  const float scale = float(shot.width) / (shot.x1 - shot.x0);
  auto depth_at = [&](float x, float y) {
    const int px = int(std::floor((x - shot.x0) * scale)), py = int(std::floor((y - shot.y0) * scale));
    if (px < 0 || py < 0 || px >= shot.width || py >= shot.height) return -std::numeric_limits<float>::infinity();
    return shot.depth[size_t(py) * shot.width + px];
  };
  // A shown anchor sits on (or just under) the surface around it: that surface is
  // its reference, so its own button cap or face never covers its line.
  float ref = anchor.z;
  if (!anchor.hidden) {
    const float reach = std::max(1.f, 1.5f / scale);
    for (float dy = -reach; dy <= reach; dy += reach)
      for (float dx = -reach; dx <= reach; dx += reach) ref = std::max(ref, depth_at(anchor.x + dx, anchor.y + dy));
  }
  const float limit = ref + shot.slack;
  const float len = std::hypot(tx - fx, ty - fy) * scale;  // in shot pixels
  const int steps = std::max(1, int(std::ceil(len * 2)));
  float open = -1;
  for (int i = 0; i <= steps; ++i) {
    const float t = float(i) / float(steps);
    const bool covered = depth_at(fx + (tx - fx) * t, fy + (ty - fy) * t) > limit;
    if (covered && open < 0) open = t;
    if (!covered && open >= 0) spans.push_back({open, t}), open = -1;
  }
  if (open >= 0) spans.push_back({open, 1.f});
  return spans;
}

std::optional<std::array<ScenePtr, 2>> installed_pair(const fs::path& left, const fs::path& right, View view,
                                                      int height) {
  auto l_model = load_model(left);
  if (!l_model) return std::nullopt;
  auto l = std::make_shared<const Model>(std::move(*l_model));
  std::shared_ptr<const Model> r_own;
  if (right != left)
    if (auto m = load_model(right)) r_own = std::make_shared<const Model>(std::move(*m));
  // One camera, chosen on the left, mirrored for the right; with a single model
  // for both hands the right picture is the left one mirrored.
  const Camera cl = view.automatic ? best_camera(*l) : fixed_camera(view);
  Camera cr = mirrored(cl);
  if (!r_own) {
    cr = cl;
    cr.r = cl.r * -1;
  }
  const Model& rm = r_own ? *r_own : *l;
  Frame fl = extent(l->mesh, cl), fr = extent(rm.mesh, cr);
  // One frame for both, mirrored: the same scale, height and baseline.
  const Frame shared{std::min(fl.x0, -fr.x1), std::max(fl.x1, -fr.x0), std::min(fl.y0, fr.y0), std::max(fl.y1, fr.y1)};
  const Frame shared_r{-shared.x1, -shared.x0, shared.y0, shared.y1};
  if (!(shared.y1 > shared.y0) || !(shared.x1 > shared.x0)) return std::nullopt;
  const float margin = 1.04f;
  const float scale = float(height) / ((shared.y1 - shared.y0) * margin);
  const int width =
      std::max(1, int(std::ceil((shared.x1 - shared.x0) * scale + float(height) * (margin - 1))));
  std::array<ScenePtr, 2> out;
  for (int hand = 0; hand < 2; ++hand) {
    const Frame& f = hand ? shared_r : shared;
    auto scene = std::make_shared<Scene>(hand && r_own ? r_own : l, hand ? cr : cl, scale, width, height,
                                         (f.x0 + f.x1) * 0.5f, (f.y0 + f.y1) * 0.5f);
    const Model& m = *scene->model;
    for (size_t p = 0; p < m.parts.size(); ++p)
      for (const auto& input : m.parts[p].inputs)
        if (std::ranges::find(scene->points, input, &Scene::Point::input) == scene->points.end())
          scene->points.push_back({input, int(p), {}});
    out[size_t(hand)] = std::move(scene);
  }
  return out;
}

ScenePtr bundled_scene(std::span<const unsigned char> glb, float side, float azimuth, float elevation, float roll,
                       int height, std::span<const Spot2> spots) {
  std::vector<std::pair<size_t, size_t>> node_range;
  auto loaded = load_glb(glb, &node_range);
  if (!loaded) return nullptr;
  // render_controller.py's camera, in glTF space (Blender's Z up is glTF's Y up).
  const float a = azimuth * kDeg, e = elevation * kDeg;
  Camera cam;
  cam.d = normalize({side * std::sin(a) * std::cos(e), std::sin(e), std::cos(a) * std::cos(e)});
  const V3 up = normalize(V3{0, 1, 0} - cam.d * cam.d.y), right = cross(up, cam.d);
  const float cr = std::cos(roll * kDeg), sr = std::sin(roll * kDeg);
  cam.r = right * cr + up * sr;
  cam.u = up * cr - right * sr;
  // The script's framing: the projected model, 5% margin, `height` tall.
  const Frame fr = extent(loaded->mesh, cam);
  if (!(fr.y1 > fr.y0) || !(fr.x1 > fr.x0)) return nullptr;
  const float scale = float(height) / ((fr.y1 - fr.y0) * 1.05f);
  const int width = std::max(16, int(std::lround(float(height) * (fr.x1 - fr.x0) / (fr.y1 - fr.y0))));

  // Each input: the named node that sits where the picture shows it, else the
  // surface under the picture's spot.
  auto doc = parse_glb(glb);
  const auto& nodes = field(doc->doc, "nodes");
  Model& model = *loaded;
  struct Match {
    std::string input;
    int node = -1;
    float x = 0, y = 0;
  };
  std::vector<Match> matches;
  const float cx = (fr.x0 + fr.x1) * 0.5f, cy = (fr.y0 + fr.y1) * 0.5f;
  auto picture = [&](V3 p) {
    return std::array<float, 2>{(dot(p, cam.r) - cx) * scale + float(width) * 0.5f,
                                float(height) * 0.5f - (dot(p, cam.u) - cy) * scale};
  };
  for (const auto& spot : spots) {
    Match best{std::string(spot.input), -1, spot.x, spot.y};
    float best_d = 6;  // pixels
    for (size_t n = 0; n < nodes.size() && n < node_range.size(); ++n) {
      if (webxr_input(nodes[n].value("name", std::string())) != spot.input) continue;
      auto [v0, v1] = node_range[n];
      if (v1 <= v0) continue;
      V3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
      for (size_t i = v0; i < v1; ++i) {
        const V3 v = model.mesh.v[i];
        lo = {std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
        hi = {std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
      }
      const auto at = picture((lo + hi) * 0.5f);
      const float d = std::hypot(at[0] - spot.x, at[1] - spot.y);
      if (d < best_d) best_d = d, best.node = int(n);
    }
    matches.push_back(best);
  }
  for (auto& m : matches)
    if (m.node >= 0) {
      model.parts.push_back({{m.input}, {node_range[size_t(m.node)]}});
      m.node = int(model.parts.size()) - 1;  // now the part
    }
  model.finish();
  model.gain = 1.5f, model.sheen = 0.01f, model.gloss = 2.2f, model.curve = 0.8f;
  auto scene = std::make_shared<Scene>(std::make_shared<const Model>(std::move(model)), cam, scale, width, height, cx, cy);
  // Surface points: what the unturned picture shows at the spot.
  std::vector<V3> screen(scene->model->mesh.v.size());
  for (size_t i = 0; i < screen.size(); ++i) {
    const V3 p = scene->model->mesh.v[i];
    const auto at = picture(p);
    screen[i] = {at[0], at[1], dot(p - scene->pivot, cam.d)};
  }
  const Raster depth = raster(*scene->model, cam, screen, width, height, false, false);
  for (const auto& m : matches) {
    if (m.node >= 0) {
      scene->points.push_back({m.input, m.node, {}});
      continue;
    }
    // The nearest covered pixel within a few pixels of the spot.
    float z = 0, best = 1e9f;
    for (int dy = -6; dy <= 6; ++dy)
      for (int dx = -6; dx <= 6; ++dx) {
        const int x = int(m.x) + dx, y = int(m.y) + dy;
        if (x < 0 || y < 0 || x >= width || y >= height) continue;
        const float d = depth.depth[size_t(y) * width + x];
        if (std::isfinite(d) && dx * dx + dy * dy < best) best = float(dx * dx + dy * dy), z = d;
      }
    const V3 at = scene->pivot + cam.r * ((m.x - scene->px) / scale) + cam.u * ((scene->py - m.y) / scale) + cam.d * z;
    scene->points.push_back({m.input, -1, at});
  }
  return scene;
}

ScenePtr without_point(const ScenePtr& scene, std::string_view input) {
  auto out = std::make_shared<Scene>(*scene);
  std::erase_if(out->points, [&](const Scene::Point& p) { return p.input == input; });
  return out;
}
ScenePtr with_points_from(const ScenePtr& target, const Scene& donor) {
  if (!target) return target;
  auto where = [](const Scene& s, const Scene::Point& p) {
    return p.part >= 0 ? s.model->parts[size_t(p.part)].center() : p.at;
  };
  std::vector<std::pair<V3, V3>> shared;  // donor, target
  std::vector<const Scene::Point*> missing;
  for (const auto& d : donor.points) {
    const auto t = std::ranges::find(target->points, d.input, &Scene::Point::input);
    if (t != target->points.end()) shared.push_back({where(donor, d), where(*target, *t)});
    else missing.push_back(&d);
  }
  if (missing.empty() || shared.size() < 3) return target;
  V3 dc{}, tc{};
  for (auto& [d, t] : shared) dc = dc + d, tc = tc + t;
  dc = dc * (1.f / float(shared.size())), tc = tc * (1.f / float(shared.size()));
  // Cross-covariance and spreads; the rotation (or reflection) is the orthogonal
  // polar factor of H^T, found by Higham's iteration X <- (X + X^-T) / 2.
  double h[3][3]{}, sd = 0, st = 0;
  for (auto& [d0, t0] : shared) {
    const V3 d = d0 - dc, t = t0 - tc;
    const float dv[3]{d.x, d.y, d.z}, tv[3]{t.x, t.y, t.z};
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) h[i][j] += double(tv[i]) * dv[j];  // X starts as H^T
    sd += dot(d, d), st += dot(t, t);
  }
  if (sd <= 0 || st <= 0) return target;
  double x[3][3];
  std::memcpy(x, h, sizeof x);
  for (int it = 0; it < 60; ++it) {
    const double det = x[0][0] * (x[1][1] * x[2][2] - x[1][2] * x[2][1]) -
                       x[0][1] * (x[1][0] * x[2][2] - x[1][2] * x[2][0]) +
                       x[0][2] * (x[1][0] * x[2][1] - x[1][1] * x[2][0]);
    if (std::abs(det) < 1e-18) return target;  // the shared inputs lie on a line
    double inv_t[3][3];  // (X^-1)^T = cofactor(X) / det
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) {
        const int i1 = (i + 1) % 3, i2 = (i + 2) % 3, j1 = (j + 1) % 3, j2 = (j + 2) % 3;
        inv_t[i][j] = (x[i1][j1] * x[i2][j2] - x[i1][j2] * x[i2][j1]) / det;
      }
    double change = 0;
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) {
        const double next = 0.5 * (x[i][j] + inv_t[i][j]);
        change += std::abs(next - x[i][j]);
        x[i][j] = next;
      }
    if (change < 1e-9) break;
  }
  const float scale = float(std::sqrt(st / sd));
  auto carry = [&](V3 p) {
    const V3 d = p - dc;
    const V3 r{float(x[0][0] * d.x + x[0][1] * d.y + x[0][2] * d.z), float(x[1][0] * d.x + x[1][1] * d.y + x[1][2] * d.z),
               float(x[2][0] * d.x + x[2][1] * d.y + x[2][2] * d.z)};
    return tc + r * scale;
  };
  auto out = std::make_shared<Scene>(*target);
  const auto& v = out->model->mesh.v;
  for (const auto* d : missing) {
    const V3 at = carry(where(donor, *d));
    // Onto the target's surface: its nearest vertex.
    float best = std::numeric_limits<float>::infinity();
    V3 on = at;
    for (const V3& p : v) {
      const V3 e = p - at;
      if (const float d2 = dot(e, e); d2 < best) best = d2, on = p;
    }
    out->points.push_back({d->input, -1, on});
  }
  return out;
}

}  // namespace venture::launcher::art
