#include "vfs.h"
#include "abi32.h"
#include "hle/libc/libc_a.h"
#include "core/address_space.h"
#include "common/log.h"
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <cctype>

namespace venture::hle::libc {
namespace {
// bionic libc/stdio/local.h __sFILE: LP64 has 32-bit _flags/_file,
// 64-bit __sbuf sizes and fpos_t; the legacy __sF stride is 152 bytes.
struct GuestFile {
  GuestAddr p;
  std::int32_t r, w, flags, fd;
  GuestAddr bf;
  std::uint64_t bf_size;
  std::int32_t line_size, pad;
  GuestAddr cookie, close, read, seek, write, ext;
  std::uint64_t ext_size;
  GuestAddr up;
  std::int32_t ur;
  unsigned char ubuf[3], nbuf;
  GuestAddr lb;
  std::uint64_t lb_size;
  std::int32_t block_size, pad2;
  std::int64_t offset;
};
static_assert(sizeof(GuestFile) == 152 && offsetof(GuestFile, fd) == 20);
// bionic libc/stdio/stdio.cpp (legacy __sFILE in NDK stdio.h): ARM
// fpos_t is 32-bit here. The inline fileno/feof/getc ABI is guest-visible.
struct GuestFile32 {
  std::uint32_t p;
  std::int32_t r, w;
  std::int16_t flags, fd;
  std::uint32_t bf; std::int32_t bf_size, line_size;
  std::uint32_t cookie, close, read, seek, write, ext; std::int32_t ext_size;
  std::uint32_t up; std::int32_t ur;
  unsigned char ubuf[3], nbuf;
  std::uint32_t lb; std::int32_t lb_size, block_size, offset;
};
static_assert(sizeof(GuestFile32) == 84 && offsetof(GuestFile32, flags) == 12 && offsetof(GuestFile32, fd) == 14);
std::size_t file_size = sizeof(GuestFile);
void initialize_file(GuestFile* p, int fd, int flags) { *p = {}; p->fd = fd; p->flags = flags; }
void initialize_file32(GuestFile* opaque, int fd, int flags) {
  auto* p = reinterpret_cast<GuestFile32*>(opaque);
  *p = {}; p->fd = static_cast<std::int16_t>(fd); p->flags = static_cast<std::int16_t>(flags);
  p->p = p->bf = h2g32(&p->nbuf); p->bf_size = 1;
}
void (*initialize_wire_file)(GuestFile*, int, int) = initialize_file;
class StreamMutex {
 public:
  void lock() {
    auto id =
        GuestThread::current() ? static_cast<unsigned>(GuestThread::current()->tid()) : UINT32_MAX;
    auto a = std::atomic_ref(owner_);
    if (a.load() == id) {
      ++depth_;
      return;
    }
    for (;;) {
      unsigned expected = 0;
      if (a.compare_exchange_weak(expected, id, std::memory_order_acquire)) {
        depth_ = 1;
        return;
      }
      if (auto* t = GuestThread::current())
        futex_wait(*t, h2g(&owner_), expected, -1);
      else
        host::yield();
    }
  }
  bool try_lock() {
    auto id = static_cast<unsigned>(GuestThread::current()->tid());
    auto a = std::atomic_ref(owner_);
    if (a.load() == id) {
      ++depth_;
      return true;
    }
    unsigned zero = 0;
    if (!a.compare_exchange_strong(zero, id, std::memory_order_acquire)) return false;
    depth_ = 1;
    return true;
  }
  void unlock() {
    if (--depth_) return;
    std::atomic_ref(owner_).store(0, std::memory_order_release);
    futex_wake(h2g(&owner_), 1);
  }

 private:
  std::uint32_t owner_ = 0, depth_ = 0;
};
struct Stream {
  GuestFile* guest;
  int fd = -1;
  std::shared_ptr<File> file;
  StreamMutex mutex;
  std::vector<unsigned char> pushed;
  bool eof = false, error = false, readable = true, writable = false, owned = true;
};
std::mutex streams_mutex;
std::map<GuestFile*, std::shared_ptr<Stream>> streams;
GuestFile* standard[3]{};
std::shared_ptr<Stream> stream(GuestFile* p) {
  std::lock_guard lock(streams_mutex);
  auto i = streams.find(p);
  if (i == streams.end()) {
    failure(err::ebadf);
    return {};
  }
  return i->second;
}
int mode_flags(const char* mode) {
  if (!mode || !*mode) return -err::einval;
  int flags;
  switch (*mode) {
    case 'r':
      flags = 0;
      break;
    case 'w':
      flags = 1 | 0100 | 01000;
      break;
    case 'a':
      flags = 1 | 0100 | 02000;
      break;
    default:
      return -err::einval;
  }
  if (std::strchr(mode, '+')) flags = (flags & ~3) | 2;
  if (std::strchr(mode, 'e')) flags |= o_cloexec;
  if (std::strchr(mode, 'x')) flags |= 0200;
  return flags;
}
GuestFile* make_stream(int fd, int flags, std::shared_ptr<File> file = {},
                       GuestFile* storage = nullptr) {
  if (!file) file = FdTable::get().file(fd);
  if (!file) {
    failure(err::ebadf);
    return nullptr;
  }
  if (!storage) {
    storage = static_cast<GuestFile*>(allocate(file_size));
    if (!storage) {
      failure(err::enomem);
      return nullptr;
    }
  }
  initialize_wire_file(storage, fd, 2 | ((flags & 3) == 0 ? 4 : (flags & 3) == 1 ? 8 : 16));
  auto s = std::make_shared<Stream>();
  s->guest = storage;
  s->fd = fd;
  s->file = std::move(file);
  s->readable = (flags & 3) != 1;
  s->writable = (flags & 3) != 0;
  std::lock_guard lock(streams_mutex);
  streams[storage] = s;
  return storage;
}
GuestFile* fopen_fn(const char* path, const char* mode) {
  int flags = mode_flags(mode);
  if (flags < 0) {
    result(flags);
    return nullptr;
  }
  int fd = fd_open(path, flags, 0666);
  if (fd < 0) {
    result(fd);
    return nullptr;
  }
  auto p = make_stream(fd, flags);
  if (!p) FdTable::get().close(fd);
  return p;
}
GuestFile* fdopen_fn(int fd, const char* mode) {
  int flags = mode_flags(mode);
  if (flags < 0) {
    result(flags);
    return nullptr;
  }
  auto file = FdTable::get().file(fd);
  if (!file) {
    failure(err::ebadf);
    return nullptr;
  }
  auto access = file->flags.load() & 3;
  if ((access == 0 && (flags & 3) != 0) || (access == 1 && (flags & 3) != 1)) {
    failure(err::einval);
    return nullptr;
  }
  if (flags & 02000) file->flags.fetch_or(02000);
  return make_stream(fd, flags, file);
}
int flush_one(Stream& s) {  // Streams currently write through; memory streams publish on sync.
  if (s.fd < 0) return static_cast<int>(result(s.file->sync()));
  return 0;
}
int fflush_fn(GuestFile* p) {
  if (!p) {
    std::vector<std::shared_ptr<Stream>> all;
    {
      std::lock_guard lock(streams_mutex);
      for (auto& [_, s] : streams) all.push_back(s);
    }
    int r = 0;
    for (auto& s : all) {
      std::lock_guard lock(s->mutex);
      if (flush_one(*s)) r = -1;
    }
    return r;
  }
  auto s = stream(p);
  if (!s) return -1;
  std::lock_guard lock(s->mutex);
  return flush_one(*s);
}
int fclose_fn(GuestFile* p) {
  auto s = stream(p);
  if (!s) return -1;
  int r;
  {
    std::lock_guard lock(s->mutex);
    r = flush_one(*s);
    if (s->fd >= 0 && FdTable::get().close(s->fd) < 0) r = failure(err::ebadf);
  }
  {
    std::lock_guard lock(streams_mutex);
    streams.erase(p);
  }
  if (s->owned) deallocate(p);
  return r;
}
GuestFile* freopen_fn(const char* path, const char* mode, GuestFile* p) {
  auto s = stream(p);
  if (!s) return nullptr;
  if (!path) {
    failure(err::enotsup);
    return nullptr;
  }
  int flags = mode_flags(mode);
  if (flags < 0) {
    result(flags);
    return nullptr;
  }
  std::lock_guard lock(s->mutex);
  flush_one(*s);
  if (s->fd >= 0) FdTable::get().close(s->fd);
  int fd = fd_open(path, flags, 0666);
  if (fd < 0) {
    result(fd);
    s->error = true;
    s->fd = -1;
    return nullptr;
  }
  s->fd = fd;
  s->file = FdTable::get().file(fd);
  s->readable = (flags & 3) != 1;
  s->writable = (flags & 3) != 0;
  s->pushed.clear();
  s->error = s->eof = false;
  initialize_wire_file(p, fd, 2 | ((flags & 3) == 0 ? 4 : (flags & 3) == 1 ? 8 : 16));
  return p;
}
std::int64_t read_bytes(Stream& s, void* p, std::uint64_t n) {
  if (!s.readable) {
    s.error = true;
    return -err::ebadf;
  }
  std::uint64_t done = 0;
  auto* bytes = static_cast<unsigned char*>(p);
  while (done < n && !s.pushed.empty()) {
    bytes[done++] = s.pushed.back();
    s.pushed.pop_back();
  }
  while (done < n) {
    auto r = s.file->read(bytes + done, n - done);
    if (trace_vfs() && s.file->path.ends_with(".apk")) log::info("vfs", "APK fread {} -> {}", n-done, r);
    if (r < 0) {
      s.error = true;
      return done ? static_cast<std::int64_t>(done) : r;
    }
    if (!r) {
      s.eof = true;
      break;
    }
    done += r;
  }
  return done;
}
std::int64_t write_bytes(Stream& s, const void* p, std::uint64_t n) {
  if (!s.writable) {
    s.error = true;
    return -err::ebadf;
  }
  std::uint64_t done = 0;
  auto* bytes = static_cast<const char*>(p);
  while (done < n) {
    auto r = s.file->write(bytes + done, n - done);
    if (r <= 0) {
      s.error = true;
      return done ? static_cast<std::int64_t>(done) : r ? r : -err::eio;
    }
    done += r;
  }
  return done;
}
template <bool Lock = true>
std::uint64_t fread_impl(void* p, std::uint64_t size, std::uint64_t count, GuestFile* f) {
  if (!size || !count) return 0;
  if (count > UINT64_MAX / size) {
    failure(err::eoverflow);
    return 0;
  }
  auto s = stream(f);
  if (!s) return 0;
  std::unique_lock lock(s->mutex, std::defer_lock);
  if constexpr (Lock) lock.lock();
  auto r = read_bytes(*s, p, size * count);
  if (r < 0) {
    result(r);
    return 0;
  }
  return r / size;
}
std::uint64_t fread_fn(void* p, std::uint64_t size, std::uint64_t count, GuestFile* f) {
  return fread_impl(p, size, count, f);
}
std::uint64_t fwrite_fn(const void* p, std::uint64_t size, std::uint64_t count, GuestFile* f) {
  if (!size || !count) return 0;
  if (count > UINT64_MAX / size) {
    failure(err::eoverflow);
    return 0;
  }
  auto s = stream(f);
  if (!s) return 0;
  std::lock_guard lock(s->mutex);
  auto r = write_bytes(*s, p, size * count);
  if (r < 0) {
    result(r);
    return 0;
  }
  return r / size;
}
int fgetc_fn(GuestFile* p) {
  unsigned char c;
  return fread_fn(&c, 1, 1, p) == 1 ? c : -1;
}
int fputc_fn(int c, GuestFile* p) {
  unsigned char b = c;
  return fwrite_fn(&b, 1, 1, p) == 1 ? b : -1;
}
int ungetc_fn(int c, GuestFile* p) {
  if (c == -1) return -1;
  auto s = stream(p);
  if (!s) return -1;
  std::lock_guard lock(s->mutex);
  s->pushed.push_back(static_cast<unsigned char>(c));
  s->eof = false;
  return static_cast<unsigned char>(c);
}
char* fgets_fn(char* out, int n, GuestFile* p) {
  if (n <= 0) return nullptr;
  auto s = stream(p);
  if (!s) return nullptr;
  std::lock_guard lock(s->mutex);
  int done = 0;
  // Pushback precedes the file's current offset and may itself end the line.
  while (done < n - 1 && !s->pushed.empty()) {
    unsigned char c;
    auto r = read_bytes(*s, &c, 1);
    if (r < 0) result(r);
    if (r != 1) { if (done) out[done] = 0; return done ? out : nullptr; }
    out[done++] = static_cast<char>(c);
    if (c == '\n') { out[done] = 0; return out; }
  }
  if (done < n - 1 && s->readable) {
    const auto capacity = n - 1 - done;
    const auto r = s->file->read_line(out + done, capacity);
    if (r != -err::enotsup) {
      if (r < 0) { s->error = true; result(r); }
      else {
        done += static_cast<int>(r);
        if (!r || (r < capacity && out[done - 1] != '\n')) s->eof = true;
      }
      if (!done) return nullptr;
      out[done] = 0;
      return out;
    }
  }
  // fgetc semantics on the stream already held, without a stream lookup per byte.
  while (done < n - 1) {
    unsigned char c;
    auto r = read_bytes(*s, &c, 1);
    if (r < 0) result(r);
    if (r != 1) break;
    out[done++] = static_cast<char>(c);
    if (c == '\n') break;
  }
  if (done == 0 && n > 1) return nullptr;
  out[done] = 0;
  return out;
}
char* fgets_chk(char* out, int n, GuestFile* p, std::uint64_t capacity) {
  // bionic/libc/bionic/fortify.cpp: negative int sizes convert to size_t
  // for __check_buffer_access, so they fail the bounds check as well.
  if (static_cast<std::uint64_t>(n) > capacity) fatal("fgets: prevented write past buffer");
  return fgets_fn(out, n, p);
}
int fputs_fn(const char* text, GuestFile* p) {
  auto n = std::strlen(text);
  return fwrite_fn(text, 1, n, p) == n ? 0 : -1;
}
int puts_fn(const char* text) {
  auto s = stream(standard[1]);
  if (!s) return -1;
  std::lock_guard lock(s->mutex);
  return fputs_fn(text, standard[1]) < 0 ? -1 : fputc_fn('\n', standard[1]);
}
int putchar_fn(int c) { return fputc_fn(c, standard[1]); }
int getchar_fn() { return fgetc_fn(standard[0]); }
std::int64_t getdelim_fn(char** out, std::uint64_t* capacity, int delimiter, GuestFile* p) {
  if (!out || !capacity) return failure(err::einval);
  auto s = stream(p);
  if (!s) return -1;
  std::lock_guard lock(s->mutex);
  std::uint64_t count = 0;
  for (;;) {
    if (!*out || *capacity < count + 2) {
      auto next = std::max<std::uint64_t>(128, count * 2 + 2);
      auto b = static_cast<char*>(reallocate(*out, next));
      if (!b) return failure(err::enomem);
      *out = b;
      *capacity = next;
    }
    auto c = fgetc_fn(p);
    if (c < 0) {
      (*out)[count] = 0;
      return count ? static_cast<std::int64_t>(count) : -1;
    }
    (*out)[count++] = c;
    (*out)[count] = 0;
    if (c == static_cast<unsigned char>(delimiter)) return count;
  }
}
std::int64_t getline_fn(char** out, std::uint64_t* cap, GuestFile* p) {
  return getdelim_fn(out, cap, '\n', p);
}
int fseek_fn(GuestFile* p, std::int64_t off, int whence) {
  auto s = stream(p);
  if (!s) return -1;
  std::lock_guard lock(s->mutex);
  if (whence == 1) off -= s->pushed.size();
  auto r = s->file->seek(off, whence);
  if (trace_vfs() && s->file->path.ends_with(".apk")) log::info("vfs", "APK fseek {} from {} -> {}", off, whence, r);
  if (r < 0) return result(r);
  s->eof = false;
  s->pushed.clear();
  return 0;
}
std::int64_t ftell_fn(GuestFile* p) {
  auto s = stream(p);
  if (!s) return -1;
  std::lock_guard lock(s->mutex);
  auto n = s->file->seek(0, 1);
  return n < 0 ? result(n) : n - static_cast<std::int64_t>(s->pushed.size());
}
int fgetpos_fn(GuestFile* p, std::int64_t* out) {
  auto r = ftell_fn(p);
  if (r < 0) return -1;
  *out = r;
  return 0;
}
int fsetpos_fn(GuestFile* p, const std::int64_t* pos) { return fseek_fn(p, *pos, 0); }
void clearerr_fn(GuestFile* p) {
  auto s = stream(p);
  if (s) {
    std::lock_guard lock(s->mutex);
    s->eof = s->error = false;
  }
}
void rewind_fn(GuestFile* p) {
  fseek_fn(p, 0, 0);
  clearerr_fn(p);
}
int feof_fn(GuestFile* p) {
  auto s = stream(p);
  if (!s) return 0;
  std::lock_guard lock(s->mutex);
  return s->eof;
}
int ferror_fn(GuestFile* p) {
  auto s = stream(p);
  if (!s) return 1;
  std::lock_guard lock(s->mutex);
  return s->error;
}
int fileno_fn(GuestFile* p) {
  auto s = stream(p);
  if (!s) return -1;
  return s->fd < 0 ? failure(err::ebadf) : s->fd;
}
int setvbuf_fn(GuestFile* p, char*, int mode, std::uint64_t) {
  if (mode < 0 || mode > 2) return failure(err::einval);
  return fflush_fn(p);
}
void setbuf_fn(GuestFile* p, char* b) { setvbuf_fn(p, b, b ? 0 : 2, 8192); }
void flockfile_fn(GuestFile* p) {
  if (auto s = stream(p)) s->mutex.lock();
}
void funlockfile_fn(GuestFile* p) {
  if (auto s = stream(p)) s->mutex.unlock();
}
int ftrylockfile_fn(GuestFile* p) {
  auto s = stream(p);
  return s && s->mutex.try_lock() ? 0 : 1;
}
template <class Cursor> using Args = PrintArgs<Cursor>;
template<class Cursor> std::string stdio_format(const char* fmt, Args<Cursor>& args) {
  if constexpr (is_a32_cursor<Cursor>) return format_printf32(fmt, args);
  else return format_printf(fmt, args);
}
template <class Cursor>
int print(GuestFile* p, const char* fmt, Cursor a) {
  Args<Cursor> args(a);
  std::string text;
  try { text = stdio_format(fmt, args); }
  catch (const PrintfError& e) { return failure(e.error); }
  if (text.size() > INT_MAX) return failure(err::eoverflow);
  return fwrite_fn(text.data(), 1, text.size(), p) == text.size() ? static_cast<int>(text.size())
                                                                  : -1;
}
int fprintf_fn(GuestFile* p, const char* fmt, VarArgs a) { return print(p, fmt, a); }
int vfprintf_fn(GuestFile* p, const char* fmt, VaList a) { return print(p, fmt, a); }
int printf_fn(const char* fmt, VarArgs a) { return print(standard[1], fmt, a); }
int vprintf_fn(const char* fmt, VaList a) { return print(standard[1], fmt, a); }
template <class Cursor>
int print_fd(int fd, const char* fmt, Cursor a) {
  Args<Cursor> args(a);
  std::string text;
  try { text = stdio_format(fmt, args); }
  catch (const PrintfError& e) { return failure(e.error); }
  auto r = fd_write(fd, text.data(), text.size());
  return r < 0 ? result(r) : static_cast<int>(r);
}
int dprintf_fn(int fd, const char* fmt, VarArgs a) { return print_fd(fd, fmt, a); }
int vdprintf_fn(int fd, const char* fmt, VaList a) { return print_fd(fd, fmt, a); }
void perror_fn(const char* text) {
  auto t = GuestThread::current();
  int e = t ? *t->errno_location() : 0;
  auto s = std::string(text ? text : "") + ": Linux errno " + std::to_string(e) + "\n";
  fwrite_fn(s.data(), 1, s.size(), standard[2]);
}
GuestFile* popen_fn(const char*, const char*) {
  log::warn("libc", "popen is unavailable in the guest process");
  failure(err::enosys);
  return nullptr;
}
class MemoryFile final : public File {
 public:
  MemoryFile(void* p, std::uint64_t size, int mode, char** published = nullptr,
             std::uint64_t* length = nullptr)
      : data_(static_cast<char*>(p)), capacity_(size), published_(published), length_(length) {
    flags = mode;
    if (published) {
      data_ = static_cast<char*>(allocate(1));
      capacity_ = 1;
      data_[0] = 0;
      owned_ = true;
      *published = data_;
      *length = 0;
    } else if (!p) {
      data_ = static_cast<char*>(allocate(size));
      if (data_) std::memset(data_, 0, size);
      owned_ = true;
    }
    if (mode & 01000) {
      used_ = 0;
      if (size) data_[0] = 0;
    } else
      used_ = size;
    if (mode & 02000) {
      used_ = std::find(data_, data_ + size, '\0') - data_;
      position_ = used_;
    }
  }
  ~MemoryFile() override {
    sync();
    if (owned_ && !published_) deallocate(data_);
  }
  std::int64_t read(void* p, std::uint64_t n) override {
    n = std::min(n, used_ - std::min(position_, used_));
    if (n) std::memcpy(p, data_ + position_, n);
    position_ += n;
    return n;
  }
  std::int64_t write(const void* p, std::uint64_t n) override {
    if (flags.load() & 02000) position_ = used_;
    if (n > UINT64_MAX - position_ - 1) return -err::eoverflow;
    if (published_ && position_ + n + 1 > capacity_) {
      auto b = static_cast<char*>(reallocate(data_, position_ + n + 1));
      if (!b) return -err::enomem;
      data_ = b;
      capacity_ = position_ + n + 1;
    }
    if (position_ > capacity_) return -err::enospc;
    n = std::min(n, capacity_ - position_);
    if (position_ > used_) std::memset(data_ + used_, 0, position_ - used_);
    std::memcpy(data_ + position_, p, n);
    position_ += n;
    used_ = std::max(used_, position_);
    return n ? n : -err::enospc;
  }
  std::int64_t seek(std::int64_t off, int w) override {
    if (w < 0 || w > 2) return -err::einval;
    std::uint64_t base = w == 0 ? 0 : w == 1 ? position_ : used_;
    if (off < -static_cast<std::int64_t>(base) || off > INT64_MAX - static_cast<std::int64_t>(base))
      return -err::einval;
    auto n = base + off;
    if (!published_ && n > capacity_) return -err::einval;
    position_ = n;
    return n;
  }
  int sync() override {
    if (capacity_) data_[std::min(used_, capacity_ - 1)] = 0;
    if (published_) {
      *published_ = data_;
      *length_ = used_;
    }
    return 0;
  }

 private:
  char* data_;
  std::uint64_t capacity_, position_ = 0, used_ = 0;
  char** published_;
  std::uint64_t* length_;
  bool owned_ = false;
};
GuestFile* fmemopen_fn(void* p, std::uint64_t n, const char* mode) {
  int flags = mode_flags(mode);
  if (flags < 0 || !n) {
    failure(err::einval);
    return nullptr;
  }
  return make_stream(-1, flags, std::make_shared<MemoryFile>(p, n, flags));
}
GuestFile* open_memstream_fn(char** p, std::uint64_t* n) {
  if (!p || !n) {
    failure(err::einval);
    return nullptr;
  }
  return make_stream(-1, 2, std::make_shared<MemoryFile>(nullptr, 0, 2, p, n));
}
GuestFile* tmpfile_fn() {
  auto root = process_config().guest_data_dir() + "/tmp-";
  for (int i = 0; i < 128; ++i) {
    std::uint64_t random;
    if (host::random(&random, 8)) return nullptr;
    auto name = root + std::to_string(random);
    int fd = fd_open(name.c_str(), 2 | 0100 | 0200, 0600);
    if (fd < 0) continue;
    MappedPath m;
    resolve_path(name, at_cwd, &m);
    host::mutate(2, m, nullptr, 0);
    return make_stream(fd, 2);
  }
  failure(err::eexist);
  return nullptr;
}
std::uint64_t fread_chk(void* p, std::uint64_t size, std::uint64_t count, GuestFile* f,
                        std::uint64_t capacity) {
  if (size && count > capacity / size) {
    failure(err::eoverflow);
    return 0;
  }
  return fread_fn(p, size, count, f);
}
std::uint64_t fwrite_chk(const void* p, std::uint64_t size, std::uint64_t count, GuestFile* f,
                         std::uint64_t capacity) {
  if (size && count > capacity / size) {
    failure(err::eoverflow);
    return 0;
  }
  return fwrite_fn(p, size, count, f);
}
template <class Cursor>
int scan(GuestFile* p, const char* fmt, Cursor cursor) {
  auto s = stream(p);
  if (!s) return -1;
  std::lock_guard lock(s->mutex);
  int assigned = 0, consumed = 0;
  auto get = [&] {
    int c = fgetc_fn(p);
    if (c >= 0) ++consumed;
    return c;
  };
  auto put = [&](int c) {
    if (c >= 0) {
      ungetc_fn(c, p);
      --consumed;
    }
  };
  auto space = [&] {
    int c;
    while ((c = get()) >= 0 && std::isspace(static_cast<unsigned char>(c))) {
    }
    put(c);
  };
  auto store_integer = [](void* out, int bytes, std::uint64_t value) {
    std::memcpy(out, &value, bytes);
  };
  for (const char* f = fmt; *f;) {
    if (std::isspace(static_cast<unsigned char>(*f))) {
      while (std::isspace(static_cast<unsigned char>(*f))) ++f;
      space();
      continue;
    }
    if (*f != '%' || f[1] == '%') {
      int c = get();
      char expected = *f;
      f += *f == '%' ? 2 : 1;
      if (c != expected) {
        put(c);
        return c < 0 && !assigned ? -1 : assigned;
      }
      continue;
    }
    ++f;
    bool suppress = *f == '*';
    if (suppress) ++f;
    int width = 0;
    while (std::isdigit(static_cast<unsigned char>(*f))) {
      if (width > 1000000) return assigned;
      width = width * 10 + (*f++ - '0');
    }
    int bytes = 4;
    bool double_precision = false;
    bool wide = false;
    if (*f == 'h') {
      ++f;
      bytes = 2;
      if (*f == 'h') {
        ++f;
        bytes = 1;
      }
    } else if (*f == 'l') {
      ++f;
      double_precision = true;
      bytes = is_a32_cursor<Cursor> ? 4 : 8;
      if (*f == 'l') { ++f; bytes = 8; }
    } else if (*f == 'j' || *f == 'z' || *f == 't') {
      bytes = *f == 'j' || !is_a32_cursor<Cursor> ? 8 : 4;
      ++f;
    } else if (*f == 'L') {
      ++f;
      wide = true;
    }
    char conversion = *f++;
    if (!conversion) return assigned;
    void* out = suppress ? nullptr : cursor.template next<void*>();
    if (conversion == 'n') {
      if (out) store_integer(out, bytes, consumed);
      continue;
    }
    if (conversion != 'c' && conversion != '[') space();
    if (conversion == 'c' || conversion == 's' || conversion == '[') {
      bool accepted[256]{};
      bool inverse = false;
      if (conversion == '[') {
        if (*f == '^') {
          inverse = true;
          ++f;
        }
        if (*f == ']') accepted[static_cast<unsigned char>(*f++)] = true;
        while (*f && *f != ']') {
          unsigned char a = *f++;
          if (*f == '-' && f[1] && f[1] != ']') {
            ++f;
            unsigned char b = *f++;
            for (unsigned n = a; n <= b; ++n) accepted[n] = true;
          } else
            accepted[a] = true;
        }
        if (*f == ']') ++f;
      }
      if (!width) width = conversion == 'c' ? 1 : INT_MAX;
      int n = 0, c = -1;
      while (n < width && (c = get()) >= 0) {
        bool match =
            conversion == 'c' || (conversion == 's' ? !std::isspace(static_cast<unsigned char>(c))
                                                    : (accepted[c] != inverse));
        if (!match) {
          put(c);
          break;
        }
        if (out) static_cast<char*>(out)[n] = c;
        ++n;
      }
      if (!n) return c < 0 && !assigned ? -1 : assigned;
      if (out) {
        if (conversion != 'c') static_cast<char*>(out)[n] = 0;
        ++assigned;
      }
      continue;
    }
    bool floating = std::strchr("aAeEfFgG", conversion) != nullptr;
    if (!floating && !std::strchr("diouxXp", conversion)) return assigned;
    if (wide && !is_a32_cursor<Cursor>) {
      failure(err::enotsup);
      return assigned;
    }
    if (!width) width = 512;
    std::string token;
    int c = -1;
    while (static_cast<int>(token.size()) < width && (c = get()) >= 0) {
      if (std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' ||
          (floating && c == '.'))
        token.push_back(c);
      else {
        put(c);
        break;
      }
    }
    if (token.empty()) return c < 0 && !assigned ? -1 : assigned;
    char* end = nullptr;
    std::uint64_t integer = 0;
    double number = 0;
    if (floating)
      number = std::strtod(token.c_str(), &end);
    else {
      int base = conversion == 'i'                                               ? 0
                 : conversion == 'o'                                             ? 8
                 : (conversion == 'x' || conversion == 'X' || conversion == 'p') ? 16
                                                                                 : 10;
      integer = std::strtoull(token.c_str(), &end, base);
    }
    auto used = static_cast<std::size_t>(end - token.c_str());
    for (std::size_t i = token.size(); i > used; --i) put(static_cast<unsigned char>(token[i - 1]));
    if (!used) return assigned;
    if (out) {
      if (floating) {
        if (double_precision || wide)
          *static_cast<double*>(out) = number;
        else
          *static_cast<float*>(out) = static_cast<float>(number);
      } else
        store_integer(out, conversion == 'p' ? (is_a32_cursor<Cursor> ? 4 : 8) : bytes, integer);
      ++assigned;
    }
  }
  return assigned;
}
int fscanf_fn(GuestFile* p, const char* fmt, VarArgs a) { return scan(p, fmt, a); }
int vfscanf_fn(GuestFile* p, const char* fmt, VaList a) { return scan(p, fmt, a); }
int scanf_fn(const char* fmt, VarArgs a) { return scan(standard[0], fmt, a); }
int vscanf_fn(const char* fmt, VaList a) { return scan(standard[0], fmt, a); }
class FunFile final : public File {
 public:
  FunFile(GuestAddr cookie, GuestAddr read, GuestAddr write, GuestAddr seek, GuestAddr close)
      : cookie_(cookie), read_(read), write_(write), seek_(seek), close_(close) {}
  ~FunFile() override {
    if (close_ && GuestThread::current()) GuestThread::current()->call(close_, {cookie_});
  }
  std::int64_t read(void* p, std::uint64_t n) override {
    if (!read_) return -err::ebadf;
    auto r = static_cast<std::int32_t>(GuestThread::current()->call(
        read_, {cookie_, h2g(p), std::min<std::uint64_t>(n, INT_MAX)}));
    return r < 0 ? -std::max(1, *GuestThread::current()->errno_location()) : r;
  }
  std::int64_t write(const void* p, std::uint64_t n) override {
    if (!write_) return -err::ebadf;
    auto r = static_cast<std::int32_t>(GuestThread::current()->call(
        write_, {cookie_, h2g(p), std::min<std::uint64_t>(n, INT_MAX)}));
    return r < 0 ? -std::max(1, *GuestThread::current()->errno_location()) : r;
  }
  std::int64_t seek(std::int64_t off, int whence) override {
    if (!seek_) return -err::espipe;
    auto r = static_cast<std::int64_t>(GuestThread::current()->call(
        seek_, {cookie_, static_cast<std::uint64_t>(off), static_cast<unsigned>(whence)}));
    return r < 0 ? -std::max(1, *GuestThread::current()->errno_location()) : r;
  }
  int sync() override { return 0; }

 private:
  GuestAddr cookie_, read_, write_, seek_, close_;
};
GuestFile* funopen_fn(GuestAddr cookie, GuestAddr read, GuestAddr write, GuestAddr seek,
                      GuestAddr close) {
  if (!read && !write) {
    failure(err::einval);
    return nullptr;
  }
  int flags = read ? (write ? 2 : 0) : 1;
  return make_stream(-1, flags, std::make_shared<FunFile>(cookie, read, write, seek, close));
}
std::mutex syslog_mutex;
std::string syslog_ident;
int syslog_options = 0;
std::atomic<int> syslog_mask{255};
void openlog_fn(const char* ident, int options, int) {
  std::lock_guard lock(syslog_mutex);
  syslog_ident = ident ? ident : "";
  syslog_options = options;
}
void closelog_fn() {
  std::lock_guard lock(syslog_mutex);
  syslog_ident.clear();
  syslog_options = 0;
}
int setlogmask_fn(int mask) { return mask ? syslog_mask.exchange(mask) : syslog_mask.load(); }
template <class Cursor>
void syslog_write(int priority, const char* fmt, Cursor a) {
  if (!(syslog_mask.load() & (1 << (priority & 7)))) return;
  Args<Cursor> args(a);
  std::string text;
  try { text = format_printf(fmt, args); }
  catch (const PrintfError&) { return; }
  std::string ident;
  {
    std::lock_guard lock(syslog_mutex);
    ident = syslog_ident;
    if (syslog_options & 1) ident += "[1000]";
  }
  log::write((priority & 7) <= 3   ? log::Level::error
             : (priority & 7) == 4 ? log::Level::warn
                                   : log::Level::info,
             ident.empty() ? "guest" : ident, text);
}
void syslog_fn(int priority, const char* fmt, VarArgs a) { syslog_write(priority, fmt, a); }
void vsyslog_fn(int priority, const char* fmt, VaList a) { syslog_write(priority, fmt, a); }
std::uint32_t getwc_fn(GuestFile* p) {
  auto s = stream(p);
  if (!s) return UINT32_MAX;
  std::lock_guard lock(s->mutex);
  int first = fgetc_fn(p);
  if (first < 0) return UINT32_MAX;
  if (first < 128) return first;
  unsigned value;
  int need;
  if (first >= 0xc2 && first <= 0xdf) {
    value = first & 31;
    need = 1;
  } else if (first >= 0xe0 && first <= 0xef) {
    value = first & 15;
    need = 2;
  } else if (first >= 0xf0 && first <= 0xf4) {
    value = first & 7;
    need = 3;
  } else {
    failure(err::eilseq);
    s->error = true;
    return UINT32_MAX;
  }
  for (int i = 0; i < need; ++i) {
    int c = fgetc_fn(p);
    if (c < 0 || (c & 0xc0) != 0x80) {
      failure(err::eilseq);
      s->error = true;
      return UINT32_MAX;
    }
    value = (value << 6) | (c & 63);
  }
  if ((need == 1 && value < 128) || (need == 2 && value < 2048) || (need == 3 && value < 65536) ||
      value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) {
    failure(err::eilseq);
    s->error = true;
    return UINT32_MAX;
  }
  return value;
}
int utf8(std::uint32_t c, unsigned char* bytes) {
  if (c < 128) {
    bytes[0] = c;
    return 1;
  }
  if (c < 2048) {
    bytes[0] = 0xc0 | (c >> 6);
    bytes[1] = 0x80 | (c & 63);
    return 2;
  }
  if (c >= 0xd800 && c <= 0xdfff) return 0;
  if (c < 65536) {
    bytes[0] = 0xe0 | (c >> 12);
    bytes[1] = 0x80 | ((c >> 6) & 63);
    bytes[2] = 0x80 | (c & 63);
    return 3;
  }
  if (c > 0x10ffff) return 0;
  bytes[0] = 0xf0 | (c >> 18);
  bytes[1] = 0x80 | ((c >> 12) & 63);
  bytes[2] = 0x80 | ((c >> 6) & 63);
  bytes[3] = 0x80 | (c & 63);
  return 4;
}
std::uint32_t putwc_fn(std::uint32_t c, GuestFile* p) {
  unsigned char bytes[4];
  int n = utf8(c, bytes);
  if (!n) {
    failure(err::eilseq);
    return UINT32_MAX;
  }
  return fwrite_fn(bytes, 1, n, p) == static_cast<unsigned>(n) ? c : UINT32_MAX;
}
std::uint32_t ungetwc_fn(std::uint32_t c, GuestFile* p) {
  unsigned char bytes[4];
  int n = utf8(c, bytes);
  if (!n) return UINT32_MAX;
  auto s = stream(p);
  if (!s) return UINT32_MAX;
  std::lock_guard lock(s->mutex);
  while (n) ungetc_fn(bytes[--n], p);
  return c;
}
int pclose_fn(GuestFile*) { return failure(err::echild); }
char* tmpnam_fn(char* out) {
  thread_local char name[4096];
  if (!out) out = name;
  for (int attempt = 0; attempt < 128; ++attempt) {
    std::uint64_t value;
    if (host::random(&value, 8)) {
      failure(err::eio);
      return nullptr;
    }
    auto path = process_config().guest_data_dir() + "/tmp-" + std::to_string(value);
    Stat s;
    if (path_stat(path.c_str(), &s) == -err::enoent) {
      if (path.size() >= 4096) {
        failure(err::enametoolong);
        return nullptr;
      }
      std::strcpy(out, path.c_str());
      return out;
    }
  }
  failure(err::eexist);
  return nullptr;
}
void mirror_file32(GuestFile* p) {
  std::shared_ptr<Stream> s;
  { std::lock_guard lock(streams_mutex); auto i = streams.find(p); if (i != streams.end()) s = i->second; }
  if (!s) return;
  std::lock_guard lock(s->mutex);
  auto* wire = reinterpret_cast<GuestFile32*>(p);
  wire->flags = static_cast<std::int16_t>(2 | (s->readable ? (s->writable ? 16 : 4) : 8) |
                                         (s->eof ? 0x20 : 0) | (s->error ? 0x40 : 0));
  wire->fd = static_cast<std::int16_t>(s->fd);
  wire->r = wire->w = 0; // unbuffered: legacy getc/putc must take the slow path
  wire->p = wire->bf = h2g32(&wire->nbuf); wire->bf_size = 1;
}
template<auto F> void stdio_adapter32(const char* name) {
  a32::register_adapter("libc.so", name, [](CallContext& c) {
    std::vector<GuestFile*> files;
    a32::ArgCursor32 cursor(c.cpu());
    for (auto type : c.info().a32->arguments) {
      auto value = cursor.next(type);
      if (type.tag.find("FILE") != std::string_view::npos && value) files.push_back(g2h<GuestFile>(value));
    }
    a32::invoke(F, c);
    for (auto p : files) mirror_file32(p);
    if (c.info().a32->result.tag.find("FILE") != std::string_view::npos && c.cpu().x(0))
      mirror_file32(g2h<GuestFile>(c.cpu().x(0)));
  });
}
int fsetpos32(GuestFile* p, const std::int32_t* pos) { return fseek_fn(p, *pos, 0); }
int fgetpos32(GuestFile* p, std::int32_t* pos) {
  auto value = ftell_fn(p);
  if (value < 0) return -1;
  if (value > INT32_MAX) return failure(err::eoverflow);
  *pos = static_cast<std::int32_t>(value); return 0;
}
int printf32(const char* fmt, a32::VarArgs32 a) { auto r = print(standard[1], fmt, a); mirror_file32(standard[1]); return r; }
int vprintf32(const char* fmt, a32::VaList32 a) { auto r = print(standard[1], fmt, a); mirror_file32(standard[1]); return r; }
int scanf32(const char* fmt, a32::VarArgs32 a) { auto r = scan(standard[0], fmt, a); mirror_file32(standard[0]); return r; }
int vscanf32(const char* fmt, a32::VaList32 a) { auto r = scan(standard[0], fmt, a); mirror_file32(standard[0]); return r; }
void register_stdio32_adapters() {
#define A(n, f) stdio_adapter32<&f>(n)
  A("fopen", fopen_fn); A("fopen64", fopen_fn); A("fdopen", fdopen_fn);
  A("freopen", freopen_fn); A("freopen64", freopen_fn); A("fclose", fclose_fn); A("fflush", fflush_fn);
  A("fread", fread_fn); A("fread_unlocked", fread_impl<false>);
  A("fwrite", fwrite_fn); A("__fread_chk", fread_chk); A("__fwrite_chk", fwrite_chk);
  A("fgetc", fgetc_fn); A("getc", fgetc_fn); A("getc_unlocked", fgetc_fn);
  A("fputc", fputc_fn); A("putc", fputc_fn); A("putc_unlocked", fputc_fn);
  A("fgets", fgets_fn); A("fputs", fputs_fn); A("ungetc", ungetc_fn);
  A("fgetc_unlocked", fgetc_fn); A("fputc_unlocked", fputc_fn); A("fgets_unlocked", fgets_fn);
  A("fputs_unlocked", fputs_fn); A("fwrite_unlocked", fwrite_fn); A("fflush_unlocked", fflush_fn);
  A("feof_unlocked", feof_fn); A("ferror_unlocked", ferror_fn); A("clearerr_unlocked", clearerr_fn);
  A("fileno_unlocked", fileno_fn);
  A("getdelim", getdelim_fn); A("getline", getline_fn);
  A("fseek", fseek_fn); A("fseeko", fseek_fn); A("fseeko64", fseek_fn);
  A("ftell", ftell_fn); A("ftello", ftell_fn); A("ftello64", ftell_fn);
  A("fgetpos", fgetpos32); A("fsetpos", fsetpos32);
  A("rewind", rewind_fn); A("setvbuf", setvbuf_fn); A("setbuf", setbuf_fn);
  A("feof", feof_fn); A("ferror", ferror_fn); A("clearerr", clearerr_fn); A("fileno", fileno_fn);
  A("flockfile", flockfile_fn); A("funlockfile", funlockfile_fn); A("ftrylockfile", ftrylockfile_fn);
  A("fmemopen", fmemopen_fn); A("tmpfile", tmpfile_fn);
  A("getwc", getwc_fn); A("fgetwc", getwc_fn); A("putwc", putwc_fn); A("fputwc", putwc_fn); A("ungetwc", ungetwc_fn);
  A("printf", printf32); A("vprintf", vprintf32);
  A("fprintf", print<a32::VarArgs32>); A("vfprintf", print<a32::VaList32>);
  A("dprintf", print_fd<a32::VarArgs32>); A("vdprintf", print_fd<a32::VaList32>);
  A("scanf", scanf32); A("vscanf", vscanf32); A("fscanf", scan<a32::VarArgs32>); A("vfscanf", scan<a32::VaList32>);
#undef A
}
}  // namespace
void flush_stdio() { fflush_fn(nullptr); }
bool flush_stdio_for_os_quit() { return fflush_fn(nullptr) == 0; }
void register_stdio(HleLibrary& lib) {
  const bool a32 = process_config().isa() == GuestIsa::a32;
  if (a32) { file_size = sizeof(GuestFile32); initialize_wire_file = initialize_file32; register_stdio32_adapters(); }
  const auto cell_size = a32 ? 4 : 8;
  auto data = AddressSpace::get().allocate(3 * file_size + 3 * cell_size, 3, "[stdio globals]");
  for (int i = 0; i < 3; ++i) {
    standard[i] = make_stream(i, i ? 1 : 0, {}, g2h<GuestFile>(data + i * file_size));
    stream(standard[i])->owned = false;
    if (a32) *g2h<std::uint32_t>(data + 3 * file_size + i * cell_size) = h2g32(standard[i]);
    else *g2h<GuestAddr>(data + 3 * file_size + i * cell_size) = h2g(standard[i]);
  }
  lib.add_data("__sF", data);
  lib.add_data("stdin", data + 3 * file_size);
  lib.add_data("stdout", data + 3 * file_size + cell_size);
  lib.add_data("stderr", data + 3 * file_size + 2 * cell_size);
#define B(n, f) VENTURE_LIBC_BIND(lib, n, f)
  B("fopen", fopen_fn);
  B("fopen64", fopen_fn);
  B("fdopen", fdopen_fn);
  B("freopen", freopen_fn);
  B("freopen64", freopen_fn);
  B("fclose", fclose_fn);
  B("fflush", fflush_fn);
  B("fread", fread_fn);
  B("fread_unlocked", fread_impl<false>);
  B("fwrite", fwrite_fn);
  B("__fread_chk", fread_chk);
  B("__fwrite_chk", fwrite_chk);
  B("fgetc", fgetc_fn);
  B("getc", fgetc_fn);
  B("getc_unlocked", fgetc_fn);
  B("fputc", fputc_fn);
  B("putc", fputc_fn);
  B("putc_unlocked", fputc_fn);
  B("fgets", fgets_fn);
  B("__fgets_chk", fgets_chk);
  B("fputs", fputs_fn);
  B("ungetc", ungetc_fn);
  B("getdelim", getdelim_fn);
  B("getline", getline_fn);
  B("fseek", fseek_fn);
  B("fseeko", fseek_fn);
  B("fseeko64", fseek_fn);
  B("ftell", ftell_fn);
  B("ftello", ftell_fn);
  B("ftello64", ftell_fn);
  B("fgetpos", fgetpos_fn);
  B("fsetpos", fsetpos_fn);
  B("rewind", rewind_fn);
  B("setvbuf", setvbuf_fn);
  B("setbuf", setbuf_fn);
  B("feof", feof_fn);
  B("ferror", ferror_fn);
  B("clearerr", clearerr_fn);
  B("fileno", fileno_fn);
  B("flockfile", flockfile_fn);
  B("ftrylockfile", ftrylockfile_fn);
  B("funlockfile", funlockfile_fn);
  B("printf", printf_fn);
  B("fprintf", fprintf_fn);
  B("vprintf", vprintf_fn);
  B("vfprintf", vfprintf_fn);
  B("dprintf", dprintf_fn);
  B("vdprintf", vdprintf_fn);
  B("puts", puts_fn);
  B("putchar", putchar_fn);
  B("getchar", getchar_fn);
  // Bionic's *_unlocked variants: streams here lock internally, so they are the same calls
  // (Unreal's FAndroidPlatformMemory::GetStats reads /proc/meminfo with fgets_unlocked).
  B("fgets_unlocked", fgets_fn);
  B("fgetc_unlocked", fgetc_fn);
  B("fputc_unlocked", fputc_fn);
  B("fputs_unlocked", fputs_fn);
  B("fwrite_unlocked", fwrite_fn);
  B("fflush_unlocked", fflush_fn);
  B("feof_unlocked", feof_fn);
  B("ferror_unlocked", ferror_fn);
  B("clearerr_unlocked", clearerr_fn);
  B("fileno_unlocked", fileno_fn);
  B("getchar_unlocked", getchar_fn);
  B("putchar_unlocked", putchar_fn);
  B("perror", perror_fn);
  B("popen", popen_fn);
  B("fmemopen", fmemopen_fn);
  B("open_memstream", open_memstream_fn);
  B("tmpfile", tmpfile_fn);
  B("fscanf", fscanf_fn);
  B("vfscanf", vfscanf_fn);
  B("scanf", scanf_fn);
  B("vscanf", vscanf_fn);
  B("funopen", funopen_fn);
  // bionic/libc/stdio/stdio.cpp: LP64 fpos_t and fpos64_t are both int64_t.
  // ARM32 needs a separate aligned-pair callback adapter; do not expose the
  // A64 callback bridge to it.
  if (!a32) B("funopen64", funopen_fn);
  B("openlog", openlog_fn);
  B("closelog", closelog_fn);
  B("setlogmask", setlogmask_fn);
  B("syslog", syslog_fn);
  B("vsyslog", vsyslog_fn);
  B("getwc", getwc_fn);
  B("fgetwc", getwc_fn);
  B("putwc", putwc_fn);
  B("fputwc", putwc_fn);
  B("ungetwc", ungetwc_fn);
  B("pclose", pclose_fn);
  B("tmpnam", tmpnam_fn);
#undef B
}
}  // namespace venture::hle::libc
