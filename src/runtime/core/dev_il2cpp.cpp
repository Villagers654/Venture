#include "core/dev_game_control.h"
#include "core/a32_types.h"
#include "core/address_space.h"
#include "core/guest_storage.h"
#include "core/linker.h"
#include "common/log.h"
#include "core/process.h"
#include "core/thread.h"
#include "platform/memory.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <vector>

namespace venture::dev_control {
struct MissingMethod : std::runtime_error { using std::runtime_error::runtime_error; };
struct Il2Cpp::Impl {
  GuestThread& thread;
  Library* library;
  unsigned width;
  std::map<std::string, GuestAddr> exports;
  // IL2CPP GC handles are uint32_t before Unity 2022.2 and pointer-sized
  // (Il2CppGCHandle) after; keep the whole register, older runtimes read w0.
  std::map<unsigned, GuestAddr> handles;
  unsigned next_handle = 1;
  std::string object_search_api;
  Impl(GuestThread& t, std::string_view name) : thread(t),
      library(Linker::get().open(name, kRtldNoLoad | kRtldNow, nullptr)),
      width(process_config().isa() == GuestIsa::a32 ? 4 : 8) {
    if (!library) throw std::runtime_error("IL2CPP library is not loaded");
    if (!call("domain_get", {})) throw std::runtime_error("IL2CPP domain is not ready");
  }
  std::uint64_t call(std::string_view suffix, std::initializer_list<std::uint64_t> args) {
    const std::string symbol = "il2cpp_" + std::string(suffix);
    auto& fn = exports[symbol];
    if (!fn) fn = Linker::get().symbol(library, symbol);
    if (!fn) throw std::runtime_error("missing API: " + symbol);
    if (width == 8) return thread.call(fn, args);
    hle::a32::CallBuilder32 packet;
    // These C APIs have only pointers, size_t and <=32-bit scalar arguments.
    // Managed 64-bit/FP arguments are memory cells passed to runtime_invoke.
    for (auto value : args) {
      if (value > UINT32_MAX) throw std::runtime_error("API argument does not fit A32");
      packet.value(static_cast<std::uint32_t>(value));
    }
    return thread.call32(fn, packet).x0;
  }
  void read(GuestAddr p, void* out, std::size_t bytes) {
    // IL2CPP vectors and GC objects can be ordinary guest malloc allocations,
    // outside the tracked ELF/mmap region map. Use the existing fault-contained
    // OS reader, with the immutable guest pointer-width bound on A32.
    if (!p || (width == 4 && (p > UINT32_MAX || bytes > (1ull << 32) - p)) ||
        !platform::read_memory(p, out, bytes)) throw std::runtime_error("unreadable API result");
  }
  GuestAddr pointer(GuestAddr p) { std::uint64_t v = 0; read(p, &v, width); return v; }
  void put_pointer(GuestAddr p, GuestAddr v) { std::memcpy(g2h(p), &v, width); }
  std::string text(GuestAddr p) {
    if (!p) return {};
    std::string value;
    for (unsigned i = 0; i < 4096; ++i) { char c; read(p + i, &c, 1); if (!c) return value; value += c; }
    throw std::runtime_error("API string exceeds 4096 bytes");
  }
  std::string type_name(GuestAddr type) {
    auto p = call("type_get_name", {type}); auto value = text(p); if (p) call("free", {p}); return value;
  }
  std::vector<GuestAddr> images() {
    GuestStorage count; count.resize(8); std::memset(g2h(count.address()), 0, 8);
    auto array = call("domain_get_assemblies", {call("domain_get", {}), count.address()});
    auto n = pointer(count.address());
    if (n > 4096) throw std::runtime_error("assembly count exceeds limit");
    std::vector<GuestAddr> result;
    for (unsigned i = 0; i < n; ++i) result.push_back(call("assembly_get_image", {pointer(array + i * width)}));
    return result;
  }
  GuestAddr klass(const Json& spec) {
    // Returned collections can be closed generic types which have no image
    // name lookup. Use the C API's concrete runtime class for such instances.
    if (!spec.contains("type") && spec.contains("instance"))
      return call("object_get_class", {object(spec["instance"])});
    const auto ns = spec.value("namespace", ""); const auto name = spec.at("type").get<std::string>();
    GuestStorage a, b; a.string(ns); b.string(name);
    GuestAddr found = 0;
    for (auto image : images()) {
      if (spec.contains("assembly") && text(call("image_get_name", {image})) != spec["assembly"].get<std::string>()) continue;
      auto c = call("class_from_name", {image, a.address(), b.address()});
      // Forwarding assemblies can resolve the same canonical class. Multiple
      // image hits are ambiguous only when they name distinct IL2CPP classes.
      if (c) { if (found && found != c) throw std::runtime_error("ambiguous type; specify assembly"); found = c; }
    }
    if (!found) throw std::runtime_error("type not found");
    return found;
  }
  unsigned retain(GuestAddr object) {
    if (!object) return 0;
    if (handles.size() >= 4096) throw std::runtime_error("object handle limit; release unused handles");
    auto id = next_handle++; handles[id] = call("gchandle_new", {object, 0}); return id;
  }
  GuestAddr object(const Json& spec) {
    auto id = spec.at("handle").get<unsigned>(); auto it = handles.find(id);
    if (it == handles.end()) throw std::runtime_error("unknown object handle");
    auto p = call("gchandle_get_target", {it->second});
    if (!p) throw std::runtime_error("object handle has no target");
    return p;
  }
  int kind(GuestAddr type) {
    int k = static_cast<int>(call("type_get_type", {type}));
    if (k == 0x11) {
      auto c = call("class_from_type", {type});
      if (call("class_is_enum", {c})) return kind(call("class_enum_basetype", {c}));
    }
    return k;
  }
  bool value_type(GuestAddr type) { return call("class_is_valuetype", {call("class_from_type", {type})}) != 0; }
  Json decode(GuestAddr type, GuestAddr address, bool boxed = false) {
    int k = kind(type);
    if (k == 1) return nullptr;
    if (boxed && address) address = call("object_unbox", {address});
    if (!address) return nullptr;
    std::uint64_t bits = 0;
    unsigned bytes = k == 2 || k == 4 || k == 5 ? 1 : k == 3 || k == 6 || k == 7 ? 2 :
        k == 8 || k == 9 || k == 12 ? 4 : k == 10 || k == 11 || k == 13 ? 8 : 0;
    if (bytes) read(address, &bits, bytes);
    switch (k) {
      case 2: return bits != 0;
      case 4: return static_cast<std::int8_t>(bits);
      case 6: return static_cast<std::int16_t>(bits);
      case 8: return static_cast<std::int32_t>(bits);
      case 10: return static_cast<std::int64_t>(bits);
      case 3: case 5: case 7: case 9: case 11: return bits;
      case 12: return std::bit_cast<float>(static_cast<std::uint32_t>(bits));
      case 13: return std::bit_cast<double>(bits);
      default: throw std::runtime_error("unsupported value result; use an object handle");
    }
  }
  Json result(GuestAddr type, GuestAddr obj) {
    if (!obj) return nullptr;
    if (kind(type) == 14) {
      auto n = call("string_length", {obj}); if (n > 65536) throw std::runtime_error("string result too long");
      std::u16string chars(n, 0); read(call("string_chars", {obj}), chars.data(), n * 2);
      // JSON's UTF-16 conversion rejects malformed guest strings safely.
      std::string utf8;
      for (std::size_t i = 0; i < chars.size(); ++i) {
        std::uint32_t c = chars[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < chars.size() && chars[i + 1] >= 0xdc00 && chars[i + 1] <= 0xdfff)
          c = 0x10000 + ((c - 0xd800) << 10) + (chars[++i] - 0xdc00);
        else if (c >= 0xd800 && c <= 0xdfff) c = 0xfffd;
        if (c < 0x80) utf8 += char(c);
        else if (c < 0x800) { utf8 += char(0xc0 | c >> 6); utf8 += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { utf8 += char(0xe0 | c >> 12); utf8 += char(0x80 | ((c >> 6) & 63)); utf8 += char(0x80 | (c & 63)); }
        else { utf8 += char(0xf0 | c >> 18); utf8 += char(0x80 | ((c >> 12) & 63)); utf8 += char(0x80 | ((c >> 6) & 63)); utf8 += char(0x80 | (c & 63)); }
      }
      return utf8;
    }
    if (value_type(type) && kind(type) != 0x11) return decode(type, obj, true);
    return {{"handle", retain(obj)}, {"type", type_name(type)}};
  }
  GuestAddr method(GuestAddr c, const Json& command) {
    const auto name = command.at("method").get<std::string>();
    const auto nargs = command.value("args", Json::array()).size();
    for (unsigned depth = 0; c && depth < 256; ++depth) {
      GuestStorage iter; iter.resize(width); put_pointer(iter.address(), 0);
      GuestAddr selected = 0;
      for (unsigned i = 0; i < 65536; ++i) {
        auto m = call("class_get_methods", {c, iter.address()}); if (!m) break;
        if (text(call("method_get_name", {m})) != name || call("method_get_param_count", {m}) != nargs) continue;
        if (command.contains("param_types")) {
          if (command["param_types"].size() != nargs) throw std::runtime_error("param_types count mismatch");
          bool match = true;
          for (unsigned p = 0; p < nargs; ++p) match &= type_name(call("method_get_param", {m, p})) == command["param_types"][p].get<std::string>();
          if (!match) continue;
        }
        if (selected) throw std::runtime_error("ambiguous method; specify param_types");
        selected = m;
      }
      // Prefer the nearest declaration; an inherited singleton or collection
      // accessor is reached only when the derived class has no matching method.
      if (selected) return selected;
      c = call("class_get_parent", {c});
    }
    throw MissingMethod("method not found: " + name);
  }
  GuestAddr argument(GuestAddr type, const Json& value, GuestStorage& cell, std::vector<unsigned>& temporary) {
    if (call("type_is_byref", {type})) throw std::runtime_error("by-reference arguments are unsupported");
    int k = kind(type); cell.resize(16); std::memset(g2h(cell.address()), 0, 16);
    std::uint64_t bits = 0;
    if (k == 14 && value.is_string()) {
      GuestStorage string; string.string(value.get<std::string>());
      auto p = call("string_new", {string.address()}); auto h = retain(p); temporary.push_back(h); return p;
    }
    if (!value_type(type) || k == 0x11) {
      if (value.is_null() && !value_type(type)) return 0;
      auto p = object(value);
      if (!call("class_is_assignable_from", {call("class_from_type", {type}), call("object_get_class", {p})}))
        throw std::runtime_error("object argument has incompatible type");
      return value_type(type) ? call("object_unbox", {p}) : p;
    }
    switch (k) {
      case 2: if (!value.is_boolean()) throw std::runtime_error("expected boolean"); bits = value.get<bool>(); break;
      case 3: case 5: case 7: case 9: case 11: {
        if (!value.is_number_integer() || (value.is_number_integer() && !value.is_number_unsigned() && value.get<std::int64_t>() < 0)) throw std::runtime_error("expected unsigned integer");
        bits = value.get<std::uint64_t>(); unsigned n = k == 5 ? 8 : k == 3 || k == 7 ? 16 : k == 9 ? 32 : 64;
        if (n != 64 && bits >= (1ull << n)) throw std::runtime_error("integer argument overflow"); break;
      }
      case 4: case 6: case 8: case 10: {
        if (!value.is_number_integer() || (value.is_number_unsigned() && value.get<std::uint64_t>() > INT64_MAX)) throw std::runtime_error("expected signed integer");
        auto v = value.get<std::int64_t>(); unsigned n = k == 4 ? 8 : k == 6 ? 16 : k == 8 ? 32 : 64;
        if (n != 64 && (v < -(1ll << (n - 1)) || v >= (1ll << (n - 1)))) throw std::runtime_error("integer argument overflow"); bits = v; break;
      }
      case 12: if (!value.is_number()) throw std::runtime_error("expected float"); bits = std::bit_cast<std::uint32_t>(value.get<float>()); break;
      case 13: if (!value.is_number()) throw std::runtime_error("expected double"); bits = std::bit_cast<std::uint64_t>(value.get<double>()); break;
      default: throw std::runtime_error("unsupported argument type");
    }
    std::memcpy(g2h(cell.address()), &bits, 8); return cell.address();
  }
  void release(unsigned id) { auto it = handles.find(id); if (it == handles.end()) throw std::runtime_error("unknown object handle"); call("gchandle_free", {it->second}); handles.erase(it); }
  Json invoke(const Json& command) {
    auto c = klass(command); auto m = method(c, command);
    GuestStorage flags; flags.resize(4);
    auto f = call("method_get_flags", {m, flags.address()});
    GuestAddr instance = command.contains("instance") ? object(command["instance"]) : 0;
    if (!(f & 0x10) && !instance) throw std::runtime_error("instance method requires instance handle");
    if ((f & 0x10) && instance) throw std::runtime_error("static method cannot have instance");
    if (instance && !call("class_is_assignable_from", {c, call("object_get_class", {instance})})) throw std::runtime_error("incompatible instance");
    if (instance && call("class_is_valuetype", {c})) instance = call("object_unbox", {instance});
    const auto args = command.value("args", Json::array());
    std::vector<GuestStorage> cells(args.size()); GuestStorage params, exception;
    params.resize(std::max<std::size_t>(1, args.size() * width)); exception.resize(width); put_pointer(exception.address(), 0);
    std::vector<unsigned> temporary;
    for (unsigned i = 0; i < args.size(); ++i) put_pointer(params.address() + i * width,
        argument(call("method_get_param", {m, i}), args[i], cells[i], temporary));
    auto ret = call("runtime_invoke", {m, instance, args.empty() ? 0 : params.address(), exception.address()});
    auto exc = pointer(exception.address());
    if (exc) {
      GuestStorage message; message.resize(4096); std::memset(g2h(message.address()), 0, 4096);
      call("format_exception", {exc, message.address(), 4096});
      for (auto id : temporary) release(id);
      throw std::runtime_error("managed exception: " + text(message.address()));
    }
    auto decoded = result(call("method_get_return_type", {m}), ret);
    for (auto id : temporary) release(id);
    return decoded;
  }
  Json unity(std::string_view type, std::string_view method_name, Json args = Json::array(), Json instance = nullptr,
             std::string_view ns = "UnityEngine.SceneManagement", Json param_types = nullptr) {
    Json c{{"type", type}, {"namespace", ns}, {"method", method_name}, {"args", args}};
    if (!instance.is_null()) c["instance"] = instance;
    if (!param_types.is_null()) c["param_types"] = param_types;
    return invoke(c);
  }
  Json execute(const Json& command) {
    const auto op = command.at("op").get<std::string>();
    if (op == "invoke") return invoke(command);
    if (op == "release") { release(command.at("handle").get<unsigned>()); return nullptr; }
    if (op == "assemblies") { Json out = Json::array(); for (auto p : images()) out.push_back(text(call("image_get_name", {p}))); return out; }
    if (op == "types") {
      Json out = Json::array(); auto filter = command.value("contains", "");
      for (auto image : images()) {
        auto image_name = text(call("image_get_name", {image}));
        if (command.contains("assembly") && command["assembly"] != image_name) continue;
        auto count = call("image_get_class_count", {image}); if (count > 200000) throw std::runtime_error("type count exceeds limit");
        for (unsigned i = 0; i < count; ++i) {
          auto c = call("image_get_class", {image, i}); if (!c) continue;
          auto n = text(call("class_get_name", {c})); auto ns = text(call("class_get_namespace", {c}));
          if ((ns + "." + n).find(filter) != std::string::npos) out.push_back({{"assembly", image_name}, {"namespace", ns}, {"type", n}});
          if (out.size() >= command.value("limit", 1000u)) return out;
        }
      }
      return out;
    }
    if (op == "methods") {
      Json out = Json::array(); GuestStorage iter; iter.resize(width); put_pointer(iter.address(), 0); auto c = klass(command);
      for (unsigned i = 0; i < 65536; ++i) {
        auto m = call("class_get_methods", {c, iter.address()}); if (!m) break;
        auto n = text(call("method_get_name", {m})); if (n.find(command.value("contains", "")) == std::string::npos) continue;
        Json args = Json::array(); auto count = call("method_get_param_count", {m}); if (count > 256) throw std::runtime_error("parameter count exceeds limit");
        for (unsigned j = 0; j < count; ++j) args.push_back(type_name(call("method_get_param", {m, j})));
        GuestStorage flags; flags.resize(4);
        out.push_back({{"method", n}, {"param_types", args}, {"return_type", type_name(call("method_get_return_type", {m}))}, {"static", (call("method_get_flags", {m, flags.address()}) & 0x10) != 0}});
      }
      return out;
    }
    if (op == "method_map") {
      // Diagnostic: the code address of every method with compiled code, to
      // name sampled guest return addresses (MethodInfo starts with it).
      Json out = Json::array(); GuestStorage iter; iter.resize(width);
      for (auto image : images()) {
        auto count = call("image_get_class_count", {image}); if (count > 200000) throw std::runtime_error("type count exceeds limit");
        for (unsigned i = 0; i < count; ++i) {
          auto c = call("image_get_class", {image, i}); if (!c) continue;
          auto type = text(call("class_get_namespace", {c})) + "." + text(call("class_get_name", {c}));
          put_pointer(iter.address(), 0);
          for (unsigned j = 0; j < 65536; ++j) {
            auto m = call("class_get_methods", {c, iter.address()}); if (!m) break;
            if (auto code = pointer(m)) out.push_back(Json::array({code, type, text(call("method_get_name", {m}))}));
          }
        }
      }
      return out;
    }
    if (op == "fields") {
      Json out = Json::array(); GuestStorage iter; iter.resize(width); put_pointer(iter.address(), 0); auto c = klass(command);
      for (unsigned i = 0; i < 65536; ++i) { auto f = call("class_get_fields", {c, iter.address()}); if (!f) break;
        out.push_back({{"field", text(call("field_get_name", {f}))}, {"type", type_name(call("field_get_type", {f}))}, {"static", (call("field_get_flags", {f}) & 0x10) != 0}}); }
      return out;
    }
    if (op == "get_field" || op == "set_field") {
      auto c = klass(command); GuestStorage n; n.string(command.at("field").get<std::string>());
      auto f = call("class_get_field_from_name", {c, n.address()}); if (!f) throw std::runtime_error("field not found");
      auto type = call("field_get_type", {f}); auto flags = call("field_get_flags", {f});
      GuestAddr instance = command.contains("instance") ? object(command["instance"]) : 0;
      if (!(flags & 0x10) && !instance) throw std::runtime_error("instance field requires instance");
      if ((flags & 0x10) && instance) throw std::runtime_error("static field cannot have instance");
      if (instance && !call("class_is_assignable_from", {c, call("object_get_class", {instance})})) throw std::runtime_error("incompatible instance");
      if (op == "get_field") return result(type, call("field_get_value_object", {f, instance}));
      if (flags & (0x20 | 0x40)) throw std::runtime_error("cannot write readonly or literal field");
      GuestStorage cell; std::vector<unsigned> refs; auto arg = argument(type, command.at("value"), cell, refs);
      // IL2CPP setters take the object itself for reference fields, and a
      // storage pointer for value fields (unlike a field getter's out slot).
      if (flags & 0x10) call("field_static_set_value", {f, arg}); else call("field_set_value", {instance, f, arg});
      for (auto id : refs) release(id); return nullptr;
    }
    if (op == "scenes") {
      auto count = unity("SceneManager", "get_sceneCountInBuildSettings").get<int>();
      if (count < 0 || count > 4096) throw std::runtime_error("scene count exceeds limit");
      Json out = Json::array(); for (int i = 0; i < count; ++i) out.push_back({{"index", i}, {"path", unity("SceneUtility", "GetScenePathByBuildIndex", Json::array({i}))}}); return out;
    }
    if (op == "scene" || op == "wait") {
      if (op == "wait" && command.contains("invoke")) {
        auto v = invoke(command["invoke"]); return {{"ready", v == command.at("equals")}, {"value", v}};
      }
      if (op == "wait" && command.contains("find")) {
        // Ready once a live object of the type exists, so a positioning step
        // follows the game's own timing instead of a guessed delay.
        auto find = command["find"]; find["op"] = "find_objects";
        auto found = execute(find); return {{"ready", !found.empty()}, {"found", found}};
      }
      auto scene = unity("SceneManager", "GetActiveScene");
      auto name = unity("Scene", "get_name", Json::array(), scene);
      // IL2CPP managed stripping can remove Scene accessors a title never uses;
      // the active scene is loaded, so a stripped isLoaded/path reports null.
      auto optional = [&](std::string_view accessor) -> Json {
        try { return unity("Scene", accessor, Json::array(), scene); } catch (const MissingMethod&) { return nullptr; }
      };
      auto loaded = optional("get_isLoaded");
      auto path = optional("get_path");
      release(scene.at("handle").get<unsigned>());
      if (op == "wait") return {{"ready", loaded != false && name == command.at("scene")}, {"scene", name}};
      return {{"name", name}, {"path", path}, {"is_loaded", loaded}};
    }
    if (op == "load_scene") {
      if (command.value("acknowledge_state_skip", false) != true) throw std::runtime_error("raw scene loads require acknowledge_state_skip:true; prefer the game API");
      const auto value = command.at("scene");
      return unity("SceneManager", "LoadSceneAsync", Json::array({value}), nullptr, "UnityEngine.SceneManagement", Json::array({value.is_string() ? "System.String" : "System.Int32"}));
    }
    if (op == "find_object") return unity("GameObject", "Find", Json::array({command.at("name")}), nullptr, "UnityEngine");
    if (op == "find_objects") {
      auto c = klass(command); auto type = call("type_get_object", {call("class_get_type", {c})});
      Json type_ref{{"handle", retain(type)}};
      const bool assets = command.value("include_assets", false);
      const bool inactive = command.value("include_inactive", false);
      Json found;
      bool filter_scene = false;
      std::string resolved;
      auto search = [&]() -> Json {
        if (!assets && !inactive) {
          try {
            auto value = unity("Object", "FindObjectsOfType", Json::array({type_ref}), nullptr,
                               "UnityEngine", Json::array({"System.Type"}));
            resolved = "Object.FindObjectsOfType(Type)";
            return value;
          } catch (const MissingMethod&) { }
          try {
            auto value = unity("Object", "FindObjectsByType", Json::array({type_ref, 0}), nullptr,
                               "UnityEngine", Json::array({"System.Type", "UnityEngine.FindObjectsSortMode"}));
            resolved = "Object.FindObjectsByType(Type,None)";
            return value;
          } catch (const MissingMethod&) { }
        }
        filter_scene = !assets;
        resolved = "Resources.FindObjectsOfTypeAll(Type)";
        return unity("Resources", "FindObjectsOfTypeAll", Json::array({type_ref}), nullptr,
                     "UnityEngine", Json::array({"System.Type"}));
      };
      try { found = search(); }
      catch (...) { release(type_ref["handle"].get<unsigned>()); throw; }
      release(type_ref["handle"].get<unsigned>());
      if (object_search_api != resolved) {
        log::info("dev-control", "find_objects resolved {}{}", resolved,
                  filter_scene ? " with scene-object filter" : "");
        object_search_api = resolved;
      }
      if (found.is_null()) return Json::array();
      Json out = Json::array();
      std::vector<unsigned> temporary;
      auto release_temporary = [&] {
        for (auto id : temporary) release(id);
        temporary.clear();
      };
      try {
        auto array = object(found); auto len = call("array_length", {array});
        if (len > 4096) throw std::runtime_error("object count exceeds limit");
        const auto go_class = filter_scene ? klass({{"namespace", "UnityEngine"}, {"type", "GameObject"}}) : 0;
        const auto component_class = filter_scene ? klass({{"namespace", "UnityEngine"}, {"type", "Component"}}) : 0;
        // Array elements start at 4 pointer words in both IL2CPP object layouts:
        // klass, monitor, bounds, max_length. Validate each returned element.
        for (unsigned i = 0; i < len; ++i) {
          auto obj = pointer(array + 4 * width + i * width); if (!obj) continue;
          auto cls = call("object_get_class", {obj});
          if (command.contains("name")) {
            Json ref{{"handle", retain(obj)}}; temporary.push_back(ref["handle"]);
            const bool matches = unity("Object", "get_name", Json::array(), ref, "UnityEngine") == command.at("name");
            release_temporary();
            if (!matches) continue;
          }
          if (filter_scene) {
            const bool go = call("class_is_assignable_from", {go_class, cls}) != 0;
            if (!go && !call("class_is_assignable_from", {component_class, cls})) continue;
            Json ref{{"handle", retain(obj)}}; temporary.push_back(ref["handle"]);
            auto game_object = ref;
            if (!go) {
              game_object = unity("Component", "get_gameObject", Json::array(), ref, "UnityEngine");
              temporary.push_back(game_object.at("handle").get<unsigned>());
            }
            auto scene = unity("GameObject", "get_scene", Json::array(), game_object, "UnityEngine");
            temporary.push_back(scene.at("handle").get<unsigned>());
            const bool valid = unity("Scene", "IsValid", Json::array(), scene).get<bool>() &&
                    (inactive || unity("GameObject", "get_activeInHierarchy", Json::array(), game_object, "UnityEngine").get<bool>());
            release_temporary();
            if (!valid) continue;
          }
          out.push_back(result(call("class_get_type", {cls}), obj));
        }
      } catch (...) {
        // A failed search transfers no handles to the caller, including matches
        // collected before a later object's managed property raised an exception.
        release_temporary();
        for (const auto& value : out) release(value.at("handle").get<unsigned>());
        release(found.at("handle").get<unsigned>());
        throw;
      }
      release(found.at("handle").get<unsigned>()); return out;
    }
    if (op == "button_click") {
      auto click = unity("Button", "get_onClick", Json::array(), command.at("instance"), "UnityEngine.UI");
      Json invoke_click{{"type", "UnityEvent"}, {"namespace", "UnityEngine.Events"}, {"method", "Invoke"}, {"instance", click}, {"args", Json::array()}};
      auto result = invoke(invoke_click); release(click.at("handle").get<unsigned>()); return result;
    }
    throw std::runtime_error("unknown operation");
  }
};
Il2Cpp::Il2Cpp(GuestThread& t, std::string_view lib) : impl_(std::make_unique<Impl>(t, lib)) {}
Il2Cpp::~Il2Cpp() = default; // No guest calls during host/process teardown.
Json Il2Cpp::execute(const Json& command) { return impl_->execute(command); }
} // namespace venture::dev_control
