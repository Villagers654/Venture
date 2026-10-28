#include "common/environment.h"
#include "core/a32_abi.h"
#include "core/error.h"
#include "core/thread.h"
#include "core/signals.h"
#include "platform/memory.h"
#include "common/log.h"
#include <algorithm>
#include <cstdlib>
#include <format>
#include <map>
#include <mutex>

namespace venture::hle::a32 {
namespace {
struct Binding { Handler handler; Checker check; };
struct Registry {
  std::mutex mutex;
  std::map<Handler, Binding> bindings;
  std::map<std::pair<std::string, std::string>, Handler> adapters;
  std::map<std::pair<std::string, std::string>, Coverage> rows;
};
Registry& registry() { static auto* r = new Registry; return *r; }
bool trace(const CallContext& ctx) {
  static bool enabled = venture::environment::getenv("VENTURE_TRACE_A32_HLE") != nullptr;
  if (!enabled) return false;
  static const char* filter = venture::environment::getenv("VENTURE_TRACE_A32_HLE_FILTER");
  if (!filter) return true;
  std::string_view remaining(filter);
  while (!remaining.empty()) {
    auto comma = remaining.find(',');
    if (remaining.substr(0, comma) == ctx.info().name) return true;
    if (comma == remaining.npos) break;
    remaining.remove_prefix(comma + 1);
  }
  return false;
}
void check_range(GuestAddr a, std::size_t size) {
  if (a < 0x10000 || a >= 0x100000000ull || size > 0x100000000ull - a) throw AbiError{err::kEFAULT};
}
std::string display(Type type, std::uint64_t bits) {
  if (type.kind == Kind::Void) return "Void";
  if (type.kind == Kind::F32) return std::format("F32:{}", std::bit_cast<float>(static_cast<std::uint32_t>(bits)));
  if (type.kind == Kind::F64) return std::format("F64:{}", std::bit_cast<double>(bits));
  if (is_signed(type.kind)) return std::format("{}:{}", kind_name(type.kind), static_cast<std::int64_t>(bits));
  return std::format("{}:{:#x}", kind_name(type.kind), bits);
}
}
std::string_view kind_name(Kind k) {
  constexpr std::string_view names[]{"Void", "I8", "U8", "I16", "U16", "I32", "U32", "I64", "U64", "F32", "F64",
    "Ptr", "Size", "SSize", "Long", "ULong", "Off", "Time", "Handle", "Callback", "OutSize", "OutLong", "OutOff", "OutTime", "OutPtr", "Layout", "Composite", "VaList", "Unsupported"};
  return names[static_cast<unsigned>(k)];
}
bool is_signed(Kind k) {
  return k == Kind::I8 || k == Kind::I16 || k == Kind::I32 || k == Kind::I64 ||
    k == Kind::SSize || k == Kind::Long || k == Kind::Off || k == Kind::Time;
}
bool is_wide(Kind k) { return k == Kind::I64 || k == Kind::U64 || k == Kind::F64; }
bool is_out(Kind k) { return k >= Kind::OutSize && k <= Kind::OutPtr; }
// Handles that Venture's own libc allocates in guest memory: a 32-bit guest
// address is their whole wire form, so no adapter or token table applies.
bool guest_memory_handle(Type t) { return t.kind == Kind::Handle && t.tag == "locale_t"; }
bool needs_adapter(Type t) {
  return t.kind == Kind::Layout || (t.kind == Kind::Handle && !guest_memory_handle(t)) || t.kind == Kind::Callback ||
    t.kind == Kind::Composite || t.kind == Kind::Unsupported;
}
std::uint64_t extend(Type t, std::uint64_t bits) {
  switch (t.kind) {
    case Kind::I8: return static_cast<std::int64_t>(static_cast<std::int8_t>(bits));
    case Kind::U8: return static_cast<std::uint8_t>(bits);
    case Kind::I16: return static_cast<std::int64_t>(static_cast<std::int16_t>(bits));
    case Kind::U16: return static_cast<std::uint16_t>(bits);
    default:
      if (is_wide(t.kind)) return bits;
      if (is_signed(t.kind)) return static_cast<std::int64_t>(static_cast<std::int32_t>(bits));
      return static_cast<std::uint32_t>(bits);
  }
}
void read_bytes(GuestAddr address, void* out, std::size_t size) {
  check_range(address, size);
  if (!platform::read_memory(address, out, size)) throw AbiError{err::kEFAULT};
}
void write_bytes(GuestAddr address, const void* data, std::size_t size) {
  check_range(address, size);
  if (!platform::write_memory(address, data, size)) throw AbiError{err::kEFAULT};
}
void check_out(GuestAddr address) {
  check_range(address, 4);
  if (!platform::memory_accessible(address, 4, true)) throw AbiError{err::kEFAULT};
}
std::uint32_t narrow_out(Type type, std::uint64_t bits) {
  bool signed_value = type.kind == Kind::OutLong || type.kind == Kind::OutOff || type.kind == Kind::OutTime;
  if (signed_value ? (static_cast<std::int64_t>(bits) < INT32_MIN || static_cast<std::int64_t>(bits) > INT32_MAX) : bits > UINT32_MAX)
    throw AbiError{err::kEOVERFLOW};
  return static_cast<std::uint32_t>(bits);
}
std::uint32_t ArgCursor32::word() {
  if (reg_ < 4) {
    const auto reg = reg_++;
    return registers_ ? registers_[reg] : static_cast<std::uint32_t>(cpu_->x(static_cast<int>(reg)));
  }
  std::uint32_t value; read_bytes(stack_, &value, 4); stack_ += 4; return value;
}
std::uint64_t ArgCursor32::pair() {
  reg_ = (reg_ + 1) & ~1u;
  if (reg_ >= 4) stack_ = align_up(stack_, 8);
  auto lo = word(); auto hi = word(); return lo | (static_cast<std::uint64_t>(hi) << 32);
}
std::uint64_t ArgCursor32::next(Type type) { return extend(type, is_wide(type.kind) ? pair() : word()); }
void ArgCursor32::composite(std::span<std::byte> out, unsigned alignment) {
  if (alignment != 4 && alignment != 8) throw std::invalid_argument("AAPCS32 composite alignment");
  if (alignment == 8) { reg_ = (reg_ + 1) & ~1u; if (reg_ == 4) stack_ = align_up(stack_, 8); }
  for (std::size_t i = 0; i < out.size(); i += 4) {
    auto w = word(); std::memcpy(out.data() + i, &w, std::min<std::size_t>(4, out.size() - i));
  }
}
void ResultWriter32::scalar(Type type, std::uint64_t bits) {
  if (type.kind == Kind::Void) return;
  if (type.kind == Kind::Ptr && bits > UINT32_MAX) throw AbiError{err::kEOVERFLOW};
  if (type.kind == Kind::Off || type.kind == Kind::Time || type.kind == Kind::SSize || type.kind == Kind::Long)
    (void)narrow_out({Kind::OutLong}, bits);
  if (type.kind == Kind::Size || type.kind == Kind::ULong) (void)narrow_out({Kind::OutSize}, bits);
  word(0, static_cast<std::uint32_t>(extend(type, bits)));
  if (is_wide(type.kind)) word(1, static_cast<std::uint32_t>(bits >> 32));
}
void ResultWriter32::composite(std::span<const std::byte> bytes) {
  if (bytes.size() > 4) write_bytes(sret_, bytes.data(), bytes.size());
  else { std::uint32_t bits = 0; std::memcpy(&bits, bytes.data(), bytes.size()); word(0, bits); }
}
std::uint64_t VaList32::take(unsigned bytes) {
  pointer_ = align_up(pointer_, bytes);
  std::uint64_t value = 0; read_bytes(pointer_, &value, bytes); pointer_ += bytes; return value;
}
void CallBuilder32::word(std::uint32_t word) {
  if (reg_ < 4) registers_[reg_++] = word; else stack_.push_back(word);
}
CallBuilder32& CallBuilder32::add(Type type, std::uint64_t bits) {
  if (needs_adapter(type) || is_out(type.kind) || type.kind == Kind::Void) throw std::invalid_argument("AAPCS32 builder needs scalar type");
  if (type.kind == Kind::Ptr && bits > UINT32_MAX) throw std::overflow_error("A32 pointer exceeds 32 bits");
  if (is_wide(type.kind)) {
    reg_ = (reg_ + 1) & ~1u;
    if (reg_ == 4 && (stack_.size() & 1)) stack_.push_back(0);
    word(static_cast<std::uint32_t>(bits)); word(static_cast<std::uint32_t>(bits >> 32));
  } else word(static_cast<std::uint32_t>(extend(type, bits)));
  return *this;
}
CallBuilder32& CallBuilder32::pointer(GuestAddr address) { return add({Kind::Ptr}, address); }
CallBuilder32& CallBuilder32::untyped_words(std::span<const std::uint64_t> words) {
  for (auto value : words) if (value > UINT32_MAX)
    throw std::overflow_error("untyped A32 argument exceeds 32 bits; use CallBuilder32");
  for (auto value : words) word(static_cast<std::uint32_t>(value));
  return *this;
}
CallBuilder32& CallBuilder32::composite(std::span<const std::byte> bytes, unsigned alignment) {
  if (alignment != 4 && alignment != 8) throw std::invalid_argument("AAPCS32 composite alignment");
  if (alignment == 8) { reg_ = (reg_ + 1) & ~1u; if (reg_ == 4 && (stack_.size() & 1)) stack_.push_back(0); }
  for (std::size_t i = 0; i < bytes.size(); i += 4) {
    std::uint32_t w = 0; std::memcpy(&w, bytes.data() + i, std::min<std::size_t>(4, bytes.size() - i)); word(w);
  }
  return *this;
}
void CallBuilder32::apply(Cpu& cpu, GuestAddr bottom) const {
  auto bytes = stack_.size() * 4;
  if (cpu.sp() < bytes) throw std::overflow_error("A32 guest stack overflow");
  auto sp = align_down(cpu.sp() - bytes, 8);
  if (sp < bottom || sp > UINT32_MAX) throw std::overflow_error("A32 guest stack overflow");
  if (bytes) write_bytes(sp, stack_.data(), bytes);
  cpu.set_sp(sp);
  for (unsigned i = 0; i < reg_; ++i) cpu.set_x(static_cast<int>(i), registers_[i]);
}
const Descriptor* descriptor(std::string_view library, std::string_view symbol) {
  // OSSDK's exported versioned factory takes a guest long; its matching
  // destructor takes that version and the returned device-manager pointer.
  if (library == "libossdk.so") {
    static constexpr Type version[]{{Kind::Long, "long"}};
    static constexpr Type manager[]{{Kind::Long, "long"}, {Kind::Ptr, "void *"}};
    static constexpr Descriptor factories[]{
        {"libossdk.so", "createVrDeviceManager", {Kind::Ptr, "void *"}, version, false, "OSSDK versioned factory ABI"},
        {"libossdk.so", "destroyVrDeviceManager", {Kind::Void, "void"}, manager, false, "OSSDK versioned factory ABI"}};
    for (const auto& entry : factories) if (entry.symbol == symbol) return &entry;
  }
  // AAudioTesting.h policy ABI, promoted to the NDK AAudio.h in API 36.
  // The base API-23 inventory predates AAudio; describe this small extension
  // explicitly rather than inferring guest widths from a host signature.
  if (library == "libaaudio.so") {
    static constexpr Type policy[]{{Kind::I32, "aaudio_policy_t"}};
    static constexpr Type stream[]{{Kind::Handle, "AAudioStream *"}};
    static constexpr Descriptor policies[]{
        {"libaaudio.so", "AAudio_setMMapPolicy", {Kind::I32, "aaudio_result_t"}, policy, false, "AAudioTesting.h"},
        {"libaaudio.so", "AAudio_getMMapPolicy", {Kind::I32, "aaudio_policy_t"}, {}, false, "AAudioTesting.h"},
        {"libaaudio.so", "AAudioStream_isMMapUsed", {Kind::U8, "bool"}, stream, false, "AAudioTesting.h"}};
    for (const auto& entry : policies) if (entry.symbol == symbol) return &entry;
  }
  // VrApi.h: const char * vrapi_GetVersionString(void). The vendor inventory
  // lists First Contact's imports only; Venture answers this query for every
  // A32 VrApi library, imported or not.
  if (library == "libvrapi.so" && symbol == "vrapi_GetVersionString") {
    static constexpr Descriptor version{"libvrapi.so", "vrapi_GetVersionString", {Kind::Ptr, "const char *", 1, 4}, {}, false, "VrApi.h"};
    return &version;
  }
  // The NDK generator groups standalone jni.h exports with JNIEnv methods.
  // ART exports the same invocation ABI; never infer it from a host signature.
  if (library == "libart.so" && symbol == "JNI_GetCreatedJavaVMs")
    return descriptor("JNIEnv", symbol);
  if (library == "libventure_java_bridge.so") {
    static constexpr Type install[]{{Kind::Handle, "jmethodID"}, {Kind::Callback, "VentureJavaCallback"},
                                     {Kind::Ptr, "void *"}, {Kind::Callback, "VentureJavaRelease"}};
    static constexpr Type remove[]{{Kind::Handle, "jmethodID"}};
    static constexpr Descriptor hooks[]{
        {"libventure_java_bridge.so", "VentureJavaInstallHook", {Kind::I32, "jvmtiError"}, install, false, "Venture hosted-Java bridge ABI"},
        {"libventure_java_bridge.so", "VentureJavaRemoveHook", {Kind::I32, "jvmtiError"}, remove, false, "Venture hosted-Java bridge ABI"}};
    for (const auto& entry : hooks) if (entry.symbol == symbol) return &entry;
  }
  for (auto table : {ndk_descriptors(), libc_descriptors(), graphics_descriptors(), vendor_descriptors()}) {
    auto key = std::pair(library, symbol);
    auto it = std::lower_bound(table.begin(), table.end(), key, [](const Descriptor& d, auto k) {
      return std::pair(d.library, d.symbol) < k;
    });
    if (it != table.end() && it->library == library && it->symbol == symbol) return &*it;
  }
  return nullptr;
}
Handler bind_handler(Handler a64, Handler a32, Checker checker) {
  auto& r = registry(); std::lock_guard lock(r.mutex);
  r.bindings.insert_or_assign(a64, Binding{a32, checker}); return a64;
}
std::string_view a64_only_reason(std::string_view library, std::string_view symbol) {
  struct Exclusion { std::string_view library, symbol, reason; };
  static constexpr Exclusion exclusions[]{
#include "core/a32_a64_only.inc"
  };
  static_assert([] {
    for (std::size_t i = 1; i < std::size(exclusions); ++i)
      if (!(std::pair(exclusions[i - 1].library, exclusions[i - 1].symbol) <
            std::pair(exclusions[i].library, exclusions[i].symbol))) return false;
    return true;
  }(), "A64-only exclusions must be strictly sorted by (library, symbol), without duplicates");
  const auto key = std::pair(library, symbol);
  const auto entry = std::lower_bound(std::begin(exclusions), std::end(exclusions), key,
      [](const Exclusion& e, auto k) { return std::pair(e.library, e.symbol) < k; });
  if (entry != std::end(exclusions) && entry->library == library && entry->symbol == symbol)
    return entry->reason;
  return {};
}
void register_adapter(std::string_view library, std::string_view symbol, Handler handler) {
  if (!handler || !descriptor(library, symbol)) throw std::invalid_argument(std::format("A32 adapter requires descriptor: {}!{}", library, symbol));
  auto& r = registry(); std::lock_guard lock(r.mutex);
  r.adapters.insert_or_assign({std::string(library), std::string(symbol)}, handler);
}
void prepare(FunctionInfo& info) {
  info.a32 = descriptor(info.library, info.name);
  auto& r = registry(); std::lock_guard lock(r.mutex);
  auto key = std::pair(info.library, info.name);
  Coverage row{info.library, info.name, info.a32 != nullptr, false, "descriptor missing"};
  const auto reason = a64_only_reason(info.library, info.name);
  if (!reason.empty())
    row.reason = "A64-only: " + std::string(reason);
  Handler selected = diagnostic;
  if (info.a32 && reason.empty()) {
    if (auto a = r.adapters.find(key); a != r.adapters.end()) { selected = a->second; row.compatible = true; row.reason = "explicit adapter"; }
    else if (auto b = r.bindings.find(info.handler); b != r.bindings.end()) {
      auto check = b->second.check(*info.a32); row.compatible = check.compatible; row.reason = check.reason;
      if (row.compatible) selected = b->second.handler;
    } else row.reason = "register-level handler requires adapter";
  }
  info.handler = selected;
  if (!row.compatible) info.execution = Execution::RunLoop; // diagnostic delivers a guest signal
  r.rows.insert_or_assign(key, std::move(row));
}
std::vector<Coverage> coverage() {
  auto& r = registry(); std::lock_guard lock(r.mutex); std::vector<Coverage> rows;
  for (auto& [key, row] : r.rows) rows.push_back(row);
  return rows;
}
void fail(CallContext& ctx, int error) {
  log::error("a32-abi", "{}!{} failed: Linux errno {}", ctx.info().library, ctx.info().name, error);
  ctx.thread().set_errno(error); ctx.return_x(UINT32_MAX); ctx.return_x1(UINT32_MAX);
}
void diagnostic(CallContext& ctx) {
  const auto* d = ctx.info().a32;
  std::string types;
  if (d) for (auto type : d->arguments) types += std::format(" {}<{}>", kind_name(type.kind), type.tag);
  log::error("a32-abi", "unsupported {}!{}({}) -> {}; descriptor {}; adapter required",
    ctx.info().library, ctx.info().name, types, d ? kind_name(d->result.kind) : "unknown", d ? "found" : "missing");
  ctx.thread().set_errno(err::kENOSYS);
  signals::deliver_synchronous(ctx.thread(), {signals::kSigSys, 1, ctx.cpu().pc() - ctx.cpu().trap_instruction_size()});
}
void trace_enter(CallContext& ctx) {
  if (!trace(ctx) || !ctx.info().a32) return;
  ArgCursor32 cursor(ctx.cpu()); std::string args;
  for (auto type : ctx.info().a32->arguments) { if (!args.empty()) args += ", "; args += display(type, cursor.next(type)); }
  // Never dereference strings, buffers, tokens or callback addresses in traces.
  log::info("a32-hle", "{}!{}({})", ctx.info().library, ctx.info().name, args);
}
void trace_leave(CallContext& ctx) {
  if (!trace(ctx) || !ctx.info().a32) return;
  auto type = ctx.info().a32->result;
  auto bits = static_cast<std::uint32_t>(ctx.x(0)) | (is_wide(type.kind) ? ctx.x(1) << 32 : 0);
  log::info("a32-hle", "{}!{} -> {}", ctx.info().library, ctx.info().name, display(type, extend(type, bits)));
}
} // namespace venture::hle::a32
