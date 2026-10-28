#include "common/environment.h"
#include <array>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>

#include "common/log.h"
#include "core/linker.h"
#include "hle/libc/libc_a.h"
#include "platform/memory.h"
namespace venture::hle::libc {
namespace {
std::atomic<int> minimum_priority{4};
std::atomic<GuestAddr> guest_logger{0}, guest_aborter{0};
std::mutex tag_mutex;
std::string default_tag = "Venture";
std::string last_error;  // guarded by tag_mutex
// system/logging/liblog/include/android/log.h (__android_log_message), LP64.
struct LogMessage {
  std::uint64_t struct_size;
  std::int32_t buffer_id, priority;
  const char* tag;
  const char* file;
  std::uint32_t line;
  const char* message;
};
static_assert(sizeof(LogMessage) == 48);
log::Level level(int p) {
  return p <= 2   ? log::Level::trace
         : p == 3 ? log::Level::debug
         : p == 4 ? log::Level::info
         : p == 5 ? log::Level::warn
                  : log::Level::error;
}
int is_loggable(int priority, const char*, int default_priority) {
  auto minimum = minimum_priority.load();
  return priority >= (minimum == 1 ? default_priority : minimum) && priority < 8;
}
int is_loggable_len(int priority, const char* tag, std::uint64_t, int default_priority) {
  return is_loggable(priority, tag, default_priority);
}
int get_minimum() { return minimum_priority.load(); }
int set_minimum(int p) { return minimum_priority.exchange(p); }
void set_logger(GuestAddr fn) { guest_logger.store(fn); }
void set_aborter(GuestAddr fn) { guest_aborter.store(fn); }
void set_tag(const char* tag) {
  std::lock_guard lock(tag_mutex);
  default_tag = tag ? tag : "Venture";
}
void host_logger(const LogMessage* m) {
  std::string tag;
  if (m->tag)
    tag = m->tag;
  else {
    std::lock_guard lock(tag_mutex);
    tag = default_tag;
  }
  std::string_view message = m->message ? m->message : "";
  std::string safe;
  // Epic 6419 libil2cpp: AndroidClient::PopMessage (0x743f064) logs entire
  // Horizon responses; AuthenticatorOculus::OnUserProofCallback (0x41b6dec)
  // and AuthenticationState::Enter.MoveNext (0x3d547b8) log proofs/identity.
  // Beat Saber 3148's analytics success line also includes account/runtime IDs.
  // Redact at the host sink; guest logger callbacks still see their input.
  if (tag == "Unity") {
    for (std::string_view prefix : {"Message data: ", "Oculus: Success getting user proof. ",
                                    "[AuthenticationState] UserID: ",
                                    "[Analytics] Analytics ID Retrieval Success."}) {
      if (auto at = message.find(prefix); at != std::string_view::npos) {
        safe = std::string(message.substr(0, at + prefix.size())) + "[Platform value redacted]";
        message = safe;
        break;
      }
    }
  }
  log::write(level(m->priority), tag, message);
  if (m->priority >= 6) {  // ANDROID_LOG_ERROR, ANDROID_LOG_FATAL
    std::lock_guard lock(tag_mutex);
    last_error = tag + ": " + std::string(message.substr(0, 400));
  }
}
void write_message(CallContext& c, const LogMessage* m) {
  if (m->struct_size < sizeof(LogMessage)) return;
  static const char* backtrace_match = venture::environment::getenv("VENTURE_LOG_BACKTRACE");
  if (backtrace_match && *backtrace_match && m->message &&
      std::string_view(m->message).find(backtrace_match) != std::string_view::npos) {
    auto& cpu = c.cpu();
    auto pc = cpu.lr(), fp = cpu.frame_pointer();
    for (int i = 0; pc && i < 48; ++i) {
      DlInfo symbol;
      Linker::get().try_address_info(pc, &symbol);
      log::info("liblog", "guest log backtrace #{:02} {}+{:#x}", i, symbol.path, pc - symbol.base);
      if (cpu.isa() == GuestIsa::a32) break;
      // AAPCS64 6.4.6: the frame record contains the previous FP and saved LR.
      // A32 requires EHABI unwinding; only its live LR is reported here.
      std::array<std::uint64_t, 2> frame{};
      if (!fp || (fp & 15) || !platform::read_memory(fp, frame.data(), sizeof(frame)) || frame[0] <= fp)
        break;
      fp = frame[0]; pc = frame[1];
    }
  }
  if (auto fn = guest_logger.load())
    c.thread().call(fn, {h2g(m)});
  else
    host_logger(m);
}
int buf_write(CallContext& c, int buffer, int priority, const char* tag, const char* text) {
  if (!is_loggable(priority, tag, 4)) return -err::kEPERM;
  LogMessage m{sizeof(LogMessage), buffer, priority, tag, nullptr, 0, text};
  write_message(c, &m);
  return 1;
}
int write(CallContext& c, int p, const char* tag, const char* text) { return buf_write(c, 0, p, tag, text); }
template <class Cursor>
int formatted(CallContext& c, int buffer, int priority, const char* tag, const char* fmt, Cursor cursor) {
  if (!is_loggable(priority, tag, 4)) return -err::kEPERM;
  PrintArgs<Cursor> args(cursor);
  try {
    auto s = [&] { if constexpr (is_a32_cursor<Cursor>) return format_printf32(fmt, args); else return format_printf(fmt, args); }();
    return buf_write(c, buffer, priority, tag, s.c_str());
  } catch (const PrintfError& e) {
    return -e.error;
  }
}
int print(CallContext& c, int p, const char* tag, const char* fmt, VarArgs a) {
  return formatted(c, 0, p, tag, fmt, a);
}
int vprint(CallContext& c, int p, const char* tag, const char* fmt, VaList a) {
  return formatted(c, 0, p, tag, fmt, a);
}
int buf_print(CallContext& c, int buffer, int p, const char* tag, const char* fmt, VarArgs a) {
  return formatted(c, buffer, p, tag, fmt, a);
}
void call_aborter(CallContext& c, const char* msg) {
  if (auto fn = guest_aborter.load())
    c.thread().call(fn, {h2g(msg)});
  else
    fatal(msg ? msg : "liblog abort");
}
void default_aborter(const char* msg) { fatal(msg ? msg : "liblog abort"); }
template<class Cursor>
void assert_with_cursor(CallContext& c, const char* condition, const char* tag, const char* fmt, Cursor a) {
  std::string message;
  if (fmt) {
    PrintArgs<Cursor> args(a);
    if constexpr (is_a32_cursor<Cursor>) message = format_printf32(fmt, args);
    else message = format_printf(fmt, args);
  } else
    message = condition ? "Assertion failed: " + std::string(condition) : "Assertion failed";
  buf_write(c, 0, 7, tag, message.c_str());
  call_aborter(c, message.c_str());
  fatal("log assert aborter returned");
}
void assert_impl(CallContext& c, const char* condition, const char* tag, const char* fmt, VarArgs a) {
  assert_with_cursor(c, condition, tag, fmt, a);
}
template<class Cursor> int print32(CallContext& c, int p, const char* tag, const char* fmt, Cursor a) {
  return formatted(c, 0, p, tag, fmt, a);
}
}  // namespace
std::string last_guest_error() {
  std::lock_guard lock(tag_mutex);
  return last_error;
}
void register_liblog(HleLibrary& lib) {
#define B(n, f) bind<&f>(lib, "__android_log_" n)
  bind_cursors<&print, &print32<a32::VarArgs32>>(lib, "__android_log_print");
  bind_cursors<&vprint, &print32<a32::VaList32>>(lib, "__android_log_vprint");
  bind_cursors<&buf_print, &formatted<a32::VarArgs32>>(lib, "__android_log_buf_print");
  bind_cursors<&assert_impl, &assert_with_cursor<a32::VarArgs32>>(lib, "__android_log_assert");
  B("write", write);
  B("buf_write", buf_write);
  B("is_loggable", is_loggable);
  B("is_loggable_len", is_loggable_len);
  B("set_minimum_priority", set_minimum);
  B("get_minimum_priority", get_minimum);
  B("set_logger", set_logger);
  B("write_log_message", write_message);
  B("set_aborter", set_aborter);
  B("call_aborter", call_aborter);
  B("default_aborter", default_aborter);
  B("set_default_tag", set_tag);
  B("stderr_logger", host_logger);
  B("logd_logger", host_logger);
#undef B
}
}  // namespace venture::hle::libc
