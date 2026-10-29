#pragma once
#include <utility>
#include <atomic>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace venture::hle::libc { class File; }
namespace venture::audio { class Stream; }
namespace venture::media {
inline constexpr int unsupported = -10002, invalid = -10004, bad_state = -10005;
// Android MediaCodecInfo.CodecCapabilities color-format identifiers.
inline constexpr int color_surface=0x7f000789, color_rgba=0x7f00a000;
struct Input {
  std::int64_t length = 0;
  std::function<std::int64_t(void*, std::uint64_t, std::int64_t)> read_at;
};
Input file_input(std::shared_ptr<hle::libc::File>, std::int64_t offset, std::int64_t length);
Input fd_input(int fd, std::int64_t offset, std::int64_t length);
Input path_input(std::string_view path);
struct Track {
  std::string mime;
  int rate = 0, channels = 0;
  int width = 0, height = 0, color_format = 0;
  std::int64_t duration_us = 0;
  std::vector<std::byte> config;
};
struct Sample {
  std::vector<std::byte> bytes;
  std::int64_t time_us = -1;
  int track = -1;
  std::uint32_t flags = 0;
  std::array<std::uint8_t,10> skip_samples{};
  bool has_skip_samples=false;
};
struct Pcm {
  std::vector<float> samples;
  int rate = 0, channels = 0;
  std::int64_t time_us = 0;
};
struct VideoFrame {
  std::vector<std::uint8_t> rgba;
  int width = 0, height = 0;
  std::int64_t time_us = 0;
};
bool video_mime(std::string_view mime);
// Packet timestamps are rounded to microseconds. Round back to the nearest
// sample when clipping the container origin/duration, avoiding one-frame loss.
void trim_pcm(Pcm&, std::int64_t begin_us, std::int64_t end_us);
class Library {
 public:
  explicit Library(const std::filesystem::path& directory);
  ~Library();
  Library(const Library&) = delete;
  bool available() const;
  static std::shared_ptr<Library> get();
  struct Impl;
  std::unique_ptr<Impl> impl;
};
class Extractor {
 public:
  explicit Extractor(std::shared_ptr<Library> library = Library::get());
  ~Extractor();
  int open(Input input);
  const std::vector<Track>& tracks() const;
  int select(int track, bool enabled = true);
  bool advance();
  const Sample& sample() const;
  int seek(std::int64_t time_us, int mode);
  void cancel();
  int error() const;
  const std::string& source_hash() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
class Decoder {
 public:
  explicit Decoder(std::shared_ptr<Library> library = Library::get());
  ~Decoder();
  int configure(const Track& track);
  int send(const Sample* sample);  // null drains EOS
  int receive(Pcm* output);       // 1 frame, 0 EAGAIN, -1 EOF, negative error
  // 2 means a picture older than discard_before_us was decoded but not
  // converted. Reference pictures are retained by FFmpeg as usual.
  int receive(VideoFrame* output,std::int64_t discard_before_us=std::numeric_limits<std::int64_t>::min(),
              int output_width=0,int output_height=0);
  std::int64_t video_conversion_us() const;
  void flush();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
// Android synchronous codec contract. Decode runs on a private worker; four
// input packets and four decoded frames bound producer/consumer backpressure.
class Codec {
 public:
  struct Info { std::int32_t offset=0,size=0;std::int64_t time=0;std::uint32_t flags=0; };
  using Sink=std::function<int(const VideoFrame&)>;
  Codec();
  ~Codec();
  void release();
  Codec(const Codec&)=delete;
  Codec& operator=(const Codec&)=delete;
  static void shutdown_all();
  int configure(const Track&, Sink = {});
  int start();
  int stop();
  int flush();
  int surface(Sink);
  std::int64_t input(std::int64_t timeout_us);
  std::span<std::byte> input_buffer(std::uint64_t index,bool require_owned=true);
  int queue(std::uint64_t index,std::uint64_t offset,std::uint64_t size,std::int64_t time,std::uint32_t flags);
  std::int64_t output(Info*,std::int64_t timeout_us);
  std::span<const std::byte> output_buffer(std::uint64_t index);
  int release(std::uint64_t index,bool render,std::int64_t at_ns=0);
  Track format() const;
 private:
  struct Impl;
  std::shared_ptr<Impl> impl;
};
enum class PlayerApi { media_player,open_sl };
class Player {
 public:
  explicit Player(PlayerApi api=PlayerApi::media_player);
  ~Player();
  int source(Input input);
  int prepare();
  int start();
  int pause();
  int stop();
  int seek(std::int64_t time_us);
  void loop(bool enabled);
  // Fixed-size window consumers can scale during color conversion. Texture
  // consumers retain the source size. Extent is queried on the video worker.
  void surface(Codec::Sink,std::function<std::pair<int,int>()> extent={});
  void volume(float left, float right);
  void release();
  bool playing() const;
  bool completed() const;
  int error() const;
  std::int64_t position_us() const;
  std::int64_t duration_us() const;
  int video_width() const;
  int video_height() const;
  // The prepared source's tracks in container order; empty before prepare.
  std::vector<Track> tracks() const;
  std::shared_ptr<audio::Stream> stream() const;
 private:
  struct Impl;
  std::shared_ptr<Impl> impl;
};
} // namespace venture::media
