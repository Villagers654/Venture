// Media NDK on 32-bit ARM guests (issue 58: Ghost Giant's Unity 2017 video player
// reached AMediaExtractor_new without an A32 descriptor and stopped on SIGSYS).
#include "android/handles32.h"
#include "android/native_window.h"
#include "core/hle_runtime.h"
#include "core/linker.h"
#include "core/thread.h"
#include "fake_cpu32.h"
#include "hle/libandroid/libandroid.h"
#include "hle/libc/vfs.h"
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>

using namespace venture;
using hle::a32::CallBuilder32;
namespace {
struct Calls {
  test::LowMemory32 memory;
  GuestAddr put(unsigned offset, const auto& value) {
    std::memcpy(memory.at<void>(offset), &value, sizeof(value));
    return memory.address + offset;
  }
  // registered_functions() returns a snapshot: keep a copy, not a pointer into it.
  std::optional<hle::FunctionInfo> find(std::string_view name) {
    static const bool registered = [] {
      hle::register_libmediandk();
      return true;
    }();
    (void)registered;
    for (auto& fn : hle::registered_functions())
      if (fn.library == "libmediandk.so" && fn.name == name) return fn;
    return std::nullopt;
  }
  std::uint64_t call(std::string_view name, const CallBuilder32& args) {
    auto fn = find(name);
    REQUIRE_MESSAGE(fn, name);
    REQUIRE_MESSAGE(fn->a32, name);
    REQUIRE_MESSAGE(fn->handler != hle::a32::diagnostic, name);
    test::FakeCpu32 cpu;
    cpu.set_sp(memory.address + 0xf000);
    args.apply(cpu, memory.address);
    hle::CallContext ctx(GuestThread::adopt_current(), cpu, *fn);
    fn->handler(ctx);
    return cpu.x(0) | (cpu.x(1) << 32);
  }
  std::uint32_t words(std::string_view name, std::initializer_list<std::uint64_t> words = {}) {
    CallBuilder32 b;
    b.untyped_words({words.begin(), words.size()});
    return std::uint32_t(call(name, b));
  }
  std::int32_t status(std::string_view name, std::initializer_list<std::uint64_t> w = {}) {
    return std::int32_t(words(name, w));
  }
};
class FixtureFile final : public hle::libc::File {
 public:
  std::vector<char> bytes;
  explicit FixtureFile(const char* path) {
    std::ifstream input(path, std::ios::binary);
    bytes.assign(std::istreambuf_iterator<char>(input), {});
  }
  std::int64_t pread(void* p, std::uint64_t size, std::int64_t off) override {
    if (off < 0 || static_cast<std::uint64_t>(off) > bytes.size()) return -1;
    auto n = std::min<std::uint64_t>(size, bytes.size() - off);
    std::memcpy(p, bytes.data() + off, n);
    return static_cast<std::int64_t>(n);
  }
  int stat(hle::libc::Stat* info) override {
    *info = {};
    info->size = bytes.size();
    info->mode = 0100400;
    return 0;
  }
};
// AOSP NdkMediaCodec.h AMediaCodecBufferInfo, as an ILP32 guest lays it out.
struct BufferInfo32 {
  std::int32_t offset, size;
  std::int64_t time;
  std::uint32_t flags;
};
static_assert(sizeof(BufferInfo32) == 24);
}  // namespace

TEST_CASE("A32 Media NDK exports are adapted, except the AHardwareBuffer accessor, which is absent") {
  Calls c;
  REQUIRE(c.find("AMediaExtractor_new"));
  unsigned adapted = 0;
  for (auto& fn : hle::registered_functions()) {
    if (fn.library != "libmediandk.so") continue;
    CHECK_MESSAGE(fn.a32, fn.name);
    if (fn.name == "AImage_getHardwareBuffer")
      CHECK(fn.handler == hle::a32::diagnostic);
    else {
      CHECK_MESSAGE(fn.handler != hle::a32::diagnostic, fn.name);
      ++adapted;
    }
  }
  CHECK(adapted >= 85);
  // On a Quest, dlsym of an absent function returns null. An export without an
  // A32 adapter is absent the same way, so probing code takes its fallback.
  auto* lib = Linker::get().open("libmediandk.so", kRtldNow, nullptr);
  REQUIRE(lib);
  CHECK(Linker::get().symbol(lib, "AMediaExtractor_new") != 0);
  CHECK(Linker::get().symbol(lib, "AMEDIAFORMAT_KEY_MIME") != 0);
  CHECK(Linker::get().symbol(lib, "AImage_getHardwareBuffer") == 0);
  CHECK(Linker::get().symbol_default("AImage_getHardwareBuffer", nullptr) == 0);
  // Format keys are const char* data: the first ILP32 word points at the text.
  auto key = Linker::get().symbol(lib, "AMEDIAFORMAT_KEY_MIME");
  REQUIRE(key);
  CHECK(std::string(g2h<const char>(*g2h<std::uint32_t>(key))) == "mime");
}

TEST_CASE("A32 extractor, format and codec decode AAC through tokens and guest-memory buffers") {
  Calls c;
  auto file = std::make_shared<FixtureFile>(VENTURE_MEDIA_FIXTURE);
  REQUIRE(file->bytes.size() > 1000);
  auto fd = hle::libc::FdTable::get().insert(file);
  REQUIRE(fd >= 0);
  auto extractor = c.words("AMediaExtractor_new");
  REQUIRE(extractor);
  CHECK(std::uint64_t(extractor) < (1ull << 32));
  // off64_t pairs: r2:r3 holds the offset, the length goes on the stack.
  REQUIRE(std::int32_t(c.call("AMediaExtractor_setDataSourceFd", CallBuilder32{}
                                                                      .value(extractor)
                                                                      .value(std::int32_t(fd))
                                                                      .value(std::int64_t(0))
                                                                      .value(std::int64_t(file->bytes.size())))) == 0);
  REQUIRE(c.words("AMediaExtractor_getTrackCount", {extractor}) == 1);
  auto format = c.words("AMediaExtractor_getTrackFormat", {extractor, 0});
  REQUIRE(format);
  CHECK(format != extractor);

  auto mime_key = c.put(0x100, "mime");
  auto mime_out = c.put(0x110, 0xfeedfaceu);
  REQUIRE(c.words("AMediaFormat_getString", {format, mime_key, mime_out}) == 1);
  auto mime = *c.memory.at<std::uint32_t>(0x110);
  REQUIRE(mime);
  CHECK(std::string(g2h<const char>(mime)) == "audio/mp4a-latm");
  auto rate_key = c.put(0x120, "sample-rate");
  auto rate = c.put(0x130, std::int32_t(0));
  REQUIRE(c.words("AMediaFormat_getInt32", {format, rate_key, rate}) == 1);
  CHECK(*c.memory.at<std::int32_t>(0x130) > 0);
  auto duration_key = c.put(0x140, "durationUs");
  auto duration = c.put(0x158, std::int64_t(0));
  REQUIRE(c.words("AMediaFormat_getInt64", {format, duration_key, duration}) == 1);
  CHECK(*c.memory.at<std::int64_t>(0x158) > 0);
  // A missing key leaves the caller's output alone, as on Android.
  auto missing_key = c.put(0x160, "no-such-key");
  CHECK(c.words("AMediaFormat_getString", {format, missing_key, mime_out}) == 0);
  CHECK(*c.memory.at<std::uint32_t>(0x110) == mime);
  // Codec-specific data and the description come back as low guest memory.
  auto csd_key = c.put(0x170, "csd-0");
  auto csd_data = c.put(0x180, 0u);
  auto csd_size = c.put(0x184, 0u);
  REQUIRE(c.words("AMediaFormat_getBuffer", {format, csd_key, csd_data, csd_size}) == 1);
  CHECK(*c.memory.at<std::uint32_t>(0x180) != 0);
  CHECK(*c.memory.at<std::uint32_t>(0x184) > 0);
  auto text = c.words("AMediaFormat_toString", {format});
  REQUIRE(text);
  CHECK(std::string(g2h<const char>(text)).find("audio/mp4a-latm") != std::string::npos);
  // float/double setters arrive in core registers under the base PCS.
  auto gain_key = c.put(0x190, "gain");
  c.call("AMediaFormat_setFloat", CallBuilder32{}.value(format).pointer(gain_key).value(0.5f));
  auto gain = c.put(0x1a0, 0.0f);
  REQUIRE(c.words("AMediaFormat_getFloat", {format, gain_key, gain}) == 1);
  CHECK(*c.memory.at<float>(0x1a0) == 0.5f);

  auto codec = c.words("AMediaCodec_createDecoderByType", {mime});
  REQUIRE(codec);
  REQUIRE(c.status("AMediaCodec_configure", {codec, format, 0, 0, 0}) == 0);
  REQUIRE(c.status("AMediaCodec_start", {codec}) == 0);
  REQUIRE(c.status("AMediaExtractor_selectTrack", {extractor, 0}) == 0);
  auto info = c.put(0x200, BufferInfo32{});
  c.put(0x200 + sizeof(BufferInfo32), 0xfeedfaceu);
  auto capacity_out = c.put(0x220, 0u);
  std::size_t decoded = 0;
  float peak = 0;
  bool eos = false;
  auto drain = [&] {
    for (int count = 0; count < 8; ++count) {
      auto index = std::int32_t(
          c.call("AMediaCodec_dequeueOutputBuffer", CallBuilder32{}.value(codec).pointer(info).value(std::int64_t(100000))));
      if (index == -2) continue;
      if (index == -1) break;
      REQUIRE(index == 0);
      auto& out = *c.memory.at<BufferInfo32>(0x200);
      auto buffer = c.words("AMediaCodec_getOutputBuffer", {codec, 0, capacity_out});
      // As on Android, the size is the buffer's capacity and an EOS output still has one.
      REQUIRE(buffer);
      REQUIRE(*c.memory.at<std::uint32_t>(0x220) >= std::uint32_t(out.size));
      if (out.size) {
        auto* pcm = g2h<std::int16_t>(buffer + out.offset);
        for (std::int32_t i = 0; i < out.size / 2; ++i) peak = std::max(peak, std::abs(pcm[i] / 32768.0f));
        decoded += out.size;
      }
      eos |= (out.flags & 4) != 0;
      REQUIRE(c.status("AMediaCodec_releaseOutputBuffer", {codec, 0, 0}) == 0);
    }
  };
  do {
    REQUIRE(std::int32_t(c.call("AMediaCodec_dequeueInputBuffer", CallBuilder32{}.value(codec).value(std::int64_t(100000)))) == 0);
    auto buffer = c.words("AMediaCodec_getInputBuffer", {codec, 0, capacity_out});
    REQUIRE(buffer);
    auto capacity = *c.memory.at<std::uint32_t>(0x220);
    REQUIRE(capacity > 0);
    auto size = std::int32_t(c.words("AMediaExtractor_readSampleData", {extractor, buffer, capacity}));
    REQUIRE(size > 0);
    auto time = std::int64_t(c.call("AMediaExtractor_getSampleTime", CallBuilder32{}.value(extractor)));
    REQUIRE(std::int32_t(c.call("AMediaCodec_queueInputBuffer", CallBuilder32{}
                                                                     .value(codec)
                                                                     .value(0u)
                                                                     .value(std::int32_t(0))
                                                                     .value(std::uint32_t(size))
                                                                     .value(std::uint64_t(time))
                                                                     .value(0u))) == 0);
    drain();
  } while (c.words("AMediaExtractor_advance", {extractor}));
  CHECK(std::int32_t(c.words("AMediaExtractor_getSampleTrackIndex", {extractor})) == -1);
  REQUIRE(std::int32_t(c.call("AMediaCodec_dequeueInputBuffer", CallBuilder32{}.value(codec).value(std::int64_t(100000)))) == 0);
  REQUIRE(std::int32_t(c.call("AMediaCodec_queueInputBuffer", CallBuilder32{}
                                                                   .value(codec)
                                                                   .value(0u)
                                                                   .value(std::int32_t(0))
                                                                   .value(0u)
                                                                   .value(std::uint64_t(250000))
                                                                   .value(4u))) == 0);
  drain();
  CHECK(eos);
  CHECK(decoded >= 48000);
  CHECK(peak > 0.05f);
  CHECK(*c.memory.at<std::uint32_t>(0x200 + sizeof(BufferInfo32)) == 0xfeedface);
  auto output_format = c.words("AMediaCodec_getOutputFormat", {codec});
  REQUIRE(output_format);
  REQUIRE(c.status("AMediaFormat_delete", {output_format}) == 0);
  REQUIRE(c.status("AMediaCodec_stop", {codec}) == 0);
  REQUIRE(c.status("AMediaCodec_delete", {codec}) == 0);
  // A deleted object's token is stale: the call fails instead of reaching freed memory.
  CHECK(c.status("AMediaCodec_start", {codec}) < 0);
  REQUIRE(c.status("AMediaFormat_delete", {format}) == 0);
  REQUIRE(c.status("AMediaExtractor_delete", {extractor}) == 0);
  // Unimplemented creation reports null, and its object calls report invalid objects.
  CHECK(c.words("AMediaCodec_createEncoderByType", {mime}) == 0);
  CHECK(c.words("AMediaMuxer_new", {std::uint64_t(fd), 0}) == 0);
  CHECK(c.status("AMediaMuxer_start", {0}) == -10003);
  hle::libc::FdTable::get().close(fd);
}

TEST_CASE("A32 image reader serves tokens, its window and CPU planes") {
  Calls c;
  auto reader_out = c.put(0x300, 0u);
  REQUIRE(c.status("AImageReader_new", {8, 4, 1, 2, reader_out}) == 0);
  auto reader = *c.memory.at<std::uint32_t>(0x300);
  REQUIRE(reader);
  auto width = c.put(0x310, 0);
  REQUIRE(c.status("AImageReader_getWidth", {reader, width}) == 0);
  CHECK(*c.memory.at<std::int32_t>(0x310) == 8);
  auto window_out = c.put(0x320, 0u);
  REQUIRE(c.status("AImageReader_getWindow", {reader, window_out}) == 0);
  auto window = *c.memory.at<std::uint32_t>(0x320);
  REQUIRE(window);
  // The listener record is two ILP32 words; a null callback installs nothing.
  auto listener = c.put(0x330, std::array<std::uint32_t, 2>{0x1234, 0});
  REQUIRE(c.status("AImageReader_setImageListener", {reader, listener}) == 0);
  auto image_out = c.put(0x340, 0u);
  CHECK(c.status("AImageReader_acquireNextImage", {reader, image_out}) == -30001);
  CHECK(*c.memory.at<std::uint32_t>(0x340) == 0);

  // A decoder's frame published to the reader's window (the window token is what
  // AMediaCodec_configure and EGL receive) becomes an AImage.
  auto pinned = android::pin_native_window(window);
  REQUIRE(pinned);
  std::vector<std::uint8_t> rgba(8 * 4 * 4, 0x40);
  REQUIRE(android::publish_window_rgba(pinned.get(), rgba, 8, 4, false, 1234) == 0);
  REQUIRE(c.status("AImageReader_acquireNextImage", {reader, image_out}) == 0);
  auto image = *c.memory.at<std::uint32_t>(0x340);
  REQUIRE(image);
  auto timestamp = c.put(0x350, std::int64_t(0));
  REQUIRE(c.status("AImage_getTimestamp", {image, timestamp}) == 0);
  CHECK(*c.memory.at<std::int64_t>(0x350) == 1234);
  auto rect = c.put(0x360, std::array<std::int32_t, 4>{-1, -1, -1, -1});
  REQUIRE(c.status("AImage_getCropRect", {image, rect}) == 0);
  CHECK(*c.memory.at<std::array<std::int32_t, 4>>(0x360) == std::array<std::int32_t, 4>{0, 0, 8, 4});
  auto data = c.put(0x370, 0u);
  auto length = c.put(0x374, 0);
  REQUIRE(c.status("AImage_getPlaneData", {image, 0, data, length}) == 0);
  auto plane = *c.memory.at<std::uint32_t>(0x370);
  REQUIRE(plane);
  CHECK(*c.memory.at<std::int32_t>(0x374) == 8 * 4 * 4);
  CHECK(*g2h<std::uint8_t>(plane) == 0x40);
  c.words("AImage_delete", {image});
  c.words("AImageReader_delete", {reader});
  CHECK(c.status("AImageReader_getWidth", {reader, width}) < 0);
}

// An AMediaDataSource as an ILP32 guest supplies it, in ARM code: readAt(void*,
// off64_t, void*, size_t) takes the offset in r2:r3, buffer and size on the stack.
TEST_CASE("A32 extractor reads an AMediaDataSource through guest callbacks and guest storage") {
  Calls c;
  std::ifstream input(VENTURE_MEDIA_FIXTURE, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
  REQUIRE(bytes.size() > 1000);
  REQUIRE(bytes.size() < 0x6000);
  std::memcpy(c.memory.at<void>(0x8000), bytes.data(), bytes.size());
  struct User { std::uint32_t data, length, reads, largest, closes, closed; };
  const auto length = std::uint32_t(bytes.size());
  auto user = c.put(0x400, User{std::uint32_t(c.memory.address + 0x8000), length, 0, 0, 0, 0});
  const std::uint32_t read_code[] = {
      0xe92d40f0,              // push {r4-r7, lr}
      0xe59d4014, 0xe59d5018,  // ldr r4, [sp, #20] (buffer); ldr r5, [sp, #24] (size)
      0xe5906004,              // ldr r6, [r0, #4] (length)
      0xe5907008, 0xe2877001, 0xe5807008,  // ++reads
      0xe590700c, 0xe1550007, 0x8580500c,  // largest = max(largest, size)
      0xe0566002, 0xb3e00000, 0xb8bd80f0,  // remaining = length - offset; negative returns -1
      0xe1550006, 0x81a05006,              // n = min(size, remaining)
      0xe5906000, 0xe0866002, 0xe1a00005,  // src = data + offset; r0 = n
      0xe2555001, 0xa4d67001, 0xa4c47001, 0xaafffffb,  // copy n bytes
      0xe8bd80f0};                                     // pop {r4-r7, pc}
  const std::uint32_t size_code[] = {
      0xe3000000 | (length >> 12 & 15) << 16 | (length & 0xfff),  // movw r0, #length
      0xe12fff1e};                                               // bx lr
  const std::uint32_t close_code[] = {
      0xe5901010, 0xe2811001, 0xe5801010,  // ++closes
      0xe5800014,                          // closed = userdata
      0xe12fff1e};
  auto read = hle::code_stub(read_code, "A32 data source readAt");
  auto size = hle::code_stub(size_code, "A32 data source getSize");
  auto close = hle::code_stub(close_code, "A32 data source close");
  auto source = c.words("AMediaDataSource_new");
  REQUIRE(source);
  c.words("AMediaDataSource_setUserdata", {source, user});
  c.words("AMediaDataSource_setReadAt", {source, read});
  c.words("AMediaDataSource_setGetSize", {source, size});
  c.words("AMediaDataSource_setClose", {source, close});
  auto extractor = c.words("AMediaExtractor_new");
  REQUIRE(extractor);
  REQUIRE(c.status("AMediaExtractor_setDataSourceCustom", {extractor, source}) == 0);
  c.words("AMediaDataSource_delete", {source});  // the extractor keeps its own copy of the callbacks
  auto& u = *c.memory.at<User>(0x400);
  CHECK(u.reads > 0);
  CHECK(u.largest > 0);
  CHECK(u.largest <= 65536);  // one guest-storage stage per call
  REQUIRE(c.words("AMediaExtractor_getTrackCount", {extractor}) == 1);
  REQUIRE(c.status("AMediaExtractor_selectTrack", {extractor, 0}) == 0);
  CHECK(std::int32_t(c.words("AMediaExtractor_readSampleData", {extractor, c.memory.address + 0x1000, 0x4000})) > 0);
  CHECK(u.closes == 0);
  REQUIRE(c.status("AMediaExtractor_delete", {extractor}) == 0);
  CHECK(u.closes == 1);
  CHECK(u.closed == user);
}
