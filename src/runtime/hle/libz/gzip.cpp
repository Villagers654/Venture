#include <zlib.h>

#include "core/os_quit.h"
#include <cstring>
#include <memory>
#include <mutex>
#include <set>

#include "core/libc_host_files.h"
#include "hle/libc/libc_a.h"
#include "platform/libc_host.h"
namespace venture::hle::libc {
static_assert(sizeof(z_off64_t) == 8);
namespace {
// Public zlib gzFile_s prefix under LP64. Keeping have=0 makes the guest's
// gzgetc macro always call our thunk instead of peeking into host buffers.
struct Gzip {
  std::uint32_t have = 0;
  unsigned char* next = nullptr;
  std::int64_t pos = 0;
  gzFile host = nullptr;
  int guest_fd = -1;
};
std::mutex gzip_mutex;
std::set<Gzip*> gzip_files;
static_assert(offsetof(Gzip, pos) == 16);
void capture_error(Gzip* file) {
  int error = 0;
  gzerror(file->host, &error);
  if (error == Z_ERRNO) set_error(platform::libc_host_error());
}
bool mode_flags(const char* mode, int& flags) {
  if (!mode) return false;
  bool found = false;
  flags = 0;
  for (auto p = mode; *p; ++p) {
    if (*p == '+') return false;
    if (*p == 'r') {
      flags = 0;
      found = true;
    }
    if (*p == 'w') {
      flags = 1 | 64 | 512;
      found = true;
    }
    if (*p == 'a') {
      flags = 1 | 64 | 1024;
      found = true;
    }
  }
  return found;
}
Gzip* adopt(int fd, const char* mode, int guest_fd) {
  auto file = std::make_unique<Gzip>();
  file->host = ::gzdopen(fd, mode);
  if (!file->host) {
    platform::close_host_fd(fd);
    set_error(err::kENOMEM);
    return nullptr;
  }
  file->guest_fd = guest_fd;
  if (os_quit::enabled) { std::lock_guard lock(gzip_mutex); gzip_files.insert(file.get()); }
  return file.release();
}
Gzip* open(const char* path, const char* mode) {
  int flags;
  if (!mode_flags(mode, flags)) {
    set_error(err::kEINVAL);
    return nullptr;
  }
  int fd = open_host_fd_for_guest(path, flags, 0666);
  return fd < 0 ? nullptr : adopt(fd, mode, -1);
}
Gzip* dopen(int guest_fd, const char* mode) {
  int flags;
  if (!mode_flags(mode, flags)) {
    set_error(err::kEINVAL);
    return nullptr;
  }
  int fd = duplicate_host_fd_for_guest(guest_fd);
  return fd < 0 ? nullptr : adopt(fd, mode, guest_fd);
}
int read(Gzip* f, void* out, std::uint32_t size) {
  auto result = gzread(f->host, out, size);
  capture_error(f);
  return result;
}
int write(Gzip* f, const void* in, std::uint32_t size) {
  auto result = gzwrite(f->host, in, size);
  capture_error(f);
  return result;
}
std::uint64_t fread(void* out, std::uint64_t size, std::uint64_t count, Gzip* f) {
  auto result = gzfread(out, size, count, f->host);
  capture_error(f);
  return result;
}
std::uint64_t fwrite(const void* in, std::uint64_t size, std::uint64_t count, Gzip* f) {
  auto result = gzfwrite(in, size, count, f->host);
  capture_error(f);
  return result;
}
int getc(Gzip* f) {
  auto result = gzgetc(f->host);
  capture_error(f);
  return result;
}
int putc(Gzip* f, int c) {
  auto result = gzputc(f->host, c);
  capture_error(f);
  return result;
}
int ungetc(int c, Gzip* f) { return gzungetc(c, f->host); }
char* gets(Gzip* f, char* out, int len) {
  auto result = gzgets(f->host, out, len);
  capture_error(f);
  return result;
}
int puts(Gzip* f, const char* in) {
  auto result = gzputs(f->host, in);
  capture_error(f);
  return result;
}
int flush(Gzip* f, int mode) {
  auto result = gzflush(f->host, mode);
  capture_error(f);
  return result;
}
int buffer(Gzip* f, std::uint32_t size) { return gzbuffer(f->host, size); }
int params(Gzip* f, int level, int strategy) { return gzsetparams(f->host, level, strategy); }
int direct(Gzip* f) { return gzdirect(f->host); }
int eof(Gzip* f) { return gzeof(f->host); }
const char* error(Gzip* f, int* out) {
  auto result = gzerror(f->host, out);
  if (out && *out == Z_ERRNO) set_error(platform::libc_host_error());
  return result;
}
void clearerr(Gzip* f) { gzclearerr(f->host); }
std::int64_t seek(Gzip* f, std::int64_t offset, int whence) {
  auto result = gzseek64(f->host, offset, whence);
  capture_error(f);
  return result;
}
std::int64_t tell(Gzip* f) { return gztell64(f->host); }
std::int64_t offset(Gzip* f) { return gzoffset64(f->host); }
int rewind(Gzip* f) {
  auto result = gzrewind(f->host);
  capture_error(f);
  return result;
}
template <auto Close>
int close(Gzip* f) {
  if (!f) return Z_STREAM_ERROR;
  if (os_quit::enabled) { std::lock_guard lock(gzip_mutex); gzip_files.erase(f); }
  auto result = Close(f->host);
  if (result == Z_ERRNO) set_error(platform::libc_host_error());
  if (f->guest_fd >= 0) close_guest_fd_for_native_library(f->guest_fd);
  delete f;
  return result;
}
template <class Cursor>
int print(Gzip* f, const char* fmt, Cursor cursor) {
  PrintArgs<Cursor> args(cursor);
  try {
    auto text = format_printf(fmt, args);
    auto n = write(f, text.data(), static_cast<unsigned>(text.size()));
    return n == 0 ? Z_BUF_ERROR : n;
  } catch (const PrintfError&) {
    return Z_BUF_ERROR;
  }
}
int printf(Gzip* f, const char* fmt, VarArgs args) { return print(f, fmt, args); }
int vprintf(Gzip* f, const char* fmt, VaList args) { return print(f, fmt, args); }
}  // namespace
bool close_gzip_for_os_quit() {
  std::set<Gzip*> files;
  { std::lock_guard lock(gzip_mutex); files.swap(gzip_files); }
  bool success = true;
  for (auto* file : files) success = close<&::gzclose>(file) == Z_OK && success;
  return success;
}
void register_gzip(HleLibrary& lib) {
#define B(n, f) bind<&f>(lib, "gz" n)
  B("open", open);
  B("open64", open);
  B("dopen", dopen);
  B("read", read);
  B("write", write);
  B("fread", fread);
  B("fwrite", fwrite);
  B("getc", getc);
  B("getc_", getc);
  B("putc", putc);
  B("ungetc", ungetc);
  B("gets", gets);
  B("puts", puts);
  B("flush", flush);
  B("buffer", buffer);
  B("setparams", params);
  B("direct", direct);
  B("eof", eof);
  B("error", error);
  B("clearerr", clearerr);
  B("seek", seek);
  B("seek64", seek);
  B("tell", tell);
  B("tell64", tell);
  B("offset", offset);
  B("offset64", offset);
  B("rewind", rewind);
  bind<&close<&::gzclose>>(lib, "gzclose");
  bind<&close<&::gzclose_r>>(lib, "gzclose_r");
  bind<&close<&::gzclose_w>>(lib, "gzclose_w");
  B("printf", printf);
  B("vprintf", vprintf);
#undef B
}
}  // namespace venture::hle::libc
