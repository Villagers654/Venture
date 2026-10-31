#include <doctest/doctest.h>
#include "media/media.h"
#include "android/native_window.h"
#include "audio/audio.h"
#include "hle/libc/vfs.h"
#include "android_test_support.h"
#include "audio_test_support.h"
#include "hle/opensles/abi.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <thread>
#include <future>
#include "../async_wait.h"

using namespace venture;
namespace {
class RegionFile final:public hle::libc::File {
 public:
  std::vector<std::byte> bytes;
  std::int64_t sequential=17;
  RegionFile() {
    std::ifstream input(VENTURE_MEDIA_FIXTURE,std::ios::binary);
    std::vector<char> tone((std::istreambuf_iterator<char>(input)),{});
    bytes.resize(tone.size()+67);
    std::memcpy(bytes.data()+31,tone.data(),tone.size());
  }
  std::int64_t pread(void* p,std::uint64_t size,std::int64_t off) override {
    if(off<0 || static_cast<std::uint64_t>(off)>bytes.size())return -1;
    auto n=std::min<std::uint64_t>(size,bytes.size()-off);
    std::memcpy(p,bytes.data()+off,n);return n;
  }
  int stat(hle::libc::Stat* info) override {*info={};info->size=bytes.size();info->mode=0100400;return 0;}
};
media::Input input(std::shared_ptr<RegionFile> file) {
  return media::file_input(file,31,file->bytes.size()-67);
}
std::int64_t call(const char* name,std::initializer_list<std::uint64_t> args={}) {
  return static_cast<std::int64_t>(test::android_call("libmediandk.so",name,args).x0);
}
void pump() {
  float output[480]{};
  audio::Backend::get().process(audio::Direction::output,output,nullptr,240);
  std::this_thread::sleep_for(std::chrono::milliseconds(6));
}
bool manual_audio_device() {
  static const bool configured=audio::Backend::get().use_null_device(false);
  return configured;
}
}
TEST_CASE("missing dynamic media libraries return unsupported without native calls") {
  auto library=std::make_shared<media::Library>(std::filesystem::absolute("missing-media-libraries"));
  CHECK_FALSE(library->available());
  auto file=std::make_shared<RegionFile>();
  media::Extractor extractor(library);
  CHECK(extractor.open(input(file))==media::unsupported);
  media::Decoder decoder(library);
  media::Track track;track.mime="audio/mp4a-latm";track.rate=48000;track.channels=2;
  CHECK(decoder.configure(track)==media::unsupported);
  CHECK(decoder.send(nullptr)==media::bad_state);
}
TEST_CASE("container duration rounding retains its last decoded sample") {
  media::Pcm pcm;pcm.rate=48000;pcm.channels=2;pcm.time_us=39978667;
  pcm.samples.assign(1024*2,0.125f);
  media::trim_pcm(pcm,0,40000000);
  CHECK(pcm.samples.size()==2048); // 1024 frames, not 1023 after microsecond rounding.
  pcm.time_us=40000000;media::trim_pcm(pcm,0,40000000);CHECK(pcm.samples.empty());
  pcm.time_us=-21333;pcm.samples.assign(2048*2,0.125f);
  media::trim_pcm(pcm,0,0);CHECK(pcm.samples.size()==2048); // Remove exactly 1024 priming frames.
}
TEST_CASE("MOV AAC uses bounded positioned HLE reads and decodes authored tone") {
  auto file=std::make_shared<RegionFile>();media::Extractor extractor;
  REQUIRE(extractor.open(input(file))==0);
  REQUIRE(extractor.tracks().size()==1);
  auto track=extractor.tracks()[0];CHECK(track.mime=="audio/mp4a-latm");
  CHECK(track.rate==48000);CHECK(track.channels==2);CHECK(track.duration_us==250000);
  REQUIRE_FALSE(track.config.empty());
  CHECK(extractor.source_hash()=="73bca38a087a9d2017d4e576d572bc0d21de3039269631234a382fe641884507");
  media::Decoder decoder;REQUIRE(decoder.configure(track)==0);
  REQUIRE(extractor.select(0)==0);
  std::size_t samples=0;float peak=0;
  do {
    REQUIRE(extractor.sample().track==0);
    CHECK((extractor.sample().flags&~1u)==0);
    REQUIRE(decoder.send(&extractor.sample())==0);
    media::Pcm pcm;
    for(int result;(result=decoder.receive(&pcm))==1;) {
      CHECK(pcm.rate==48000);CHECK(pcm.channels==2);samples+=pcm.samples.size();
      for(auto value:pcm.samples)peak=std::max(peak,std::abs(value));
    }
  } while(extractor.advance());
  CHECK(samples>=24000);CHECK(peak>0.05f);CHECK(peak<0.2f);
  CHECK(file->sequential==17);
  CHECK(extractor.sample().track==-1);CHECK(extractor.sample().flags==0);
  REQUIRE(extractor.seek(100000,0)==0);CHECK(extractor.sample().time_us<=100000);
  CHECK(extractor.select(7)==media::invalid);
  REQUIRE(extractor.select(0,false)==0);CHECK_FALSE(extractor.advance());
  media::Extractor bad;
  auto invalid=input(file);invalid.length=16;
  CHECK(bad.open(invalid)==media::unsupported);
  CHECK_FALSE(media::file_input(file,INT64_MAX,4).read_at);
}
TEST_CASE("NDK extractor flags and MediaCodec buffers carry AAC to caller owned PCM") {
  for(auto key:{"AAC_PROFILE","MAX_INPUT_SIZE","CSD_0","COLOR_TRANSFER","ENCODER_PADDING"})
    REQUIRE(test::android_has_symbol("libmediandk.so",std::string("AMEDIAFORMAT_KEY_")+key));
  auto file=std::make_shared<RegionFile>();auto fd=hle::libc::FdTable::get().insert(file);
  auto extractor=static_cast<GuestAddr>(call("AMediaExtractor_new"));REQUIRE(extractor);
  REQUIRE(call("AMediaExtractor_setDataSourceFd",{extractor,static_cast<std::uint64_t>(fd),31,file->bytes.size()-67})==0);
  REQUIRE(call("AMediaExtractor_getTrackCount",{extractor})==1);
  auto format=static_cast<GuestAddr>(call("AMediaExtractor_getTrackFormat",{extractor,0}));REQUIRE(format);
  const char* mime="audio/mp4a-latm";
  auto codec=static_cast<GuestAddr>(call("AMediaCodec_createDecoderByType",{h2g(mime)}));REQUIRE(codec);
  REQUIRE(call("AMediaCodec_configure",{codec,format,0,0,0})==0);
  REQUIRE(call("AMediaCodec_start",{codec})==0);
  REQUIRE(call("AMediaExtractor_selectTrack",{extractor,0})==0);
  struct Info {std::int32_t offset,size;std::int64_t time;std::uint32_t flags;} info{};
  std::size_t decoded=0;float peak=0;bool eos=false;
  auto drain=[&] {
    for(int count=0;count<8;++count) {
      auto index=call("AMediaCodec_dequeueOutputBuffer",{codec,h2g(&info),100000});
      if(index==-2)continue;if(index==-1)break;REQUIRE(index==0);
      std::uint64_t size=0;auto buffer=call("AMediaCodec_getOutputBuffer",{codec,0,h2g(&size)});
      REQUIRE(size==static_cast<std::uint64_t>(info.size));
      if(info.size) {
        REQUIRE(buffer);auto pcm=g2h<std::int16_t>(buffer);
        for(std::size_t i=0;i<size/2;++i)peak=std::max(peak,std::abs(pcm[i]/32768.0f));
        decoded+=size;
      }
      eos|=(info.flags&4)!=0;
      REQUIRE(call("AMediaCodec_releaseOutputBuffer",{codec,0,0})==0);
    }
  };
  do {
    CHECK(call("AMediaExtractor_getSampleTrackIndex",{extractor})==0);
    CHECK(call("AMediaExtractor_getSampleFlags",{extractor})==1);
    REQUIRE(call("AMediaCodec_dequeueInputBuffer",{codec,100000})==0);
    std::uint64_t capacity=0;auto buffer=static_cast<GuestAddr>(call("AMediaCodec_getInputBuffer",{codec,0,h2g(&capacity)}));REQUIRE(buffer);
    auto size=call("AMediaExtractor_readSampleData",{extractor,buffer,capacity});REQUIRE(size>0);
    auto time=call("AMediaExtractor_getSampleTime",{extractor});
    CHECK(call("AMediaCodec_queueInputBuffer",{codec,0,UINT64_MAX,1,0,0})==media::invalid);
    REQUIRE(call("AMediaCodec_queueInputBuffer",{codec,0,0,static_cast<std::uint64_t>(size),static_cast<std::uint64_t>(time),0})==0);
    drain();
  } while(call("AMediaExtractor_advance",{extractor}));
  CHECK(call("AMediaExtractor_getSampleFlags",{extractor})==0);
  CHECK(call("AMediaExtractor_getSampleTrackIndex",{extractor})==-1);
  REQUIRE(call("AMediaCodec_dequeueInputBuffer",{codec,100000})==0);
  REQUIRE(call("AMediaCodec_queueInputBuffer",{codec,0,0,0,250000,4})==0);drain();
  CHECK(eos);CHECK(decoded>=48000);CHECK(peak>0.05f);
  CHECK(file->sequential==17);
  REQUIRE(call("AMediaCodec_flush",{codec})==0);
  REQUIRE(call("AMediaCodec_stop",{codec})==0);
  REQUIRE(call("AMediaCodec_delete",{codec})==0);
  CHECK(call("AMediaCodec_start",{codec})==-10003);
  REQUIRE(call("AMediaFormat_delete",{format})==0);
  REQUIRE(call("AMediaExtractor_delete",{extractor})==0);
  CHECK(call("AMediaExtractor_getSampleFlags",{extractor})==0);
  hle::libc::FdTable::get().close(fd);
}
TEST_CASE("audio player prepares seeks loops drains PCM and releases bounded worker") {
  REQUIRE(manual_audio_device());
  auto file=std::make_shared<RegionFile>();media::Player player;
  REQUIRE(player.source(input(file))==0);REQUIRE(player.prepare()==0);
  auto stream=player.stream();REQUIRE(stream);
  // The null device has no autonomous consumer. Advance it only when owned
  // PCM is ready; host worker scheduling is not the clock of this test.
  std::vector<float> queued, delivered;
  auto pull_ready=[&](std::uint32_t limit=240, bool capture=false) {
    const auto deadline=std::chrono::steady_clock::now()+venture::test::hang_timeout;
    for (;;) {
      REQUIRE(player.error()==0);
      if(player.completed())return false;
      const auto available=stream->ring.readable();
      const bool eos=stream->source_eos.load(std::memory_order_acquire);
      if(available>=240 || (eos && available)) {
        std::uint32_t contiguous=0;
        const auto* input=stream->ring.read_pointer(&contiguous);
        const auto frames=std::min({available,limit,contiguous});
        REQUIRE(frames>0);
        if(capture)queued.insert(queued.end(),input,input+frames*2);
        float output[480]{};
        audio::Backend::get().process(audio::Direction::output,output,nullptr,frames);
        if(capture)delivered.insert(delivered.end(),output,output+frames*2);
        return true;
      }
      REQUIRE(std::chrono::steady_clock::now()<deadline);
      std::this_thread::yield();
    }
  };
  CHECK(player.duration_us()==250000);player.loop(true);REQUIRE(player.start()==0);
  const auto loop_origin=stream->frames_read.load();
  while(stream->frames_read.load()-loop_origin<18000)
    REQUIRE(pull_ready(std::min<std::int64_t>(240,18000-(stream->frames_read.load()-loop_origin))));
  CHECK(stream->frames_read.load()-loop_origin==18000); // 375 ms crosses a loop.
  CHECK(player.playing());CHECK_FALSE(player.completed());
  REQUIRE(player.pause()==0);auto position=player.position_us();
  float paused[480]{};
  audio::Backend::get().process(audio::Direction::output,paused,nullptr,240);
  CHECK(player.position_us()==position);
  CHECK(std::all_of(std::begin(paused),std::end(paused),[](float value){return value==0;}));
  REQUIRE(player.seek(70000)==0);CHECK(player.position_us()==70000);

  // Snapshot each contiguous queued segment before consuming it. The manual
  // consumer owns the read side, so the producer cannot overwrite these samples.
  // Compare delivery sample-for-sample, including the short final segment, without
  // using a separately initialized AAC decoder as an oracle for decoder history.
  const auto seek_origin=stream->frames_read.load();
  player.loop(false);REQUIRE(player.start()==0);
  while(pull_ready(240,true)) {}
  CHECK(player.completed());CHECK_FALSE(player.playing());
  CHECK(player.position_us()==250000);
  CHECK(stream->frames_read.load()-seek_origin==8640);
  REQUIRE(queued.size()==8640*2);
  REQUIRE(delivered.size()==queued.size());
  CHECK(std::equal(delivered.begin(),delivered.end(),queued.begin()));
  CHECK(stream->non_silent_written>0);
  CHECK(stream->xruns==0);CHECK(stream->missing_frames==0);
  player.release();player.release();CHECK_FALSE(player.playing());
  CHECK(player.start()==media::bad_state);
}
TEST_CASE("NDK missing libraries returns logged unsupported" * doctest::skip()) {
  REQUIRE_FALSE(media::Library::get()->available());
  CHECK(call("AMediaCodec_createDecoderByType",{h2g("audio/mp4a-latm")})==0);
  auto file=std::make_shared<RegionFile>();auto fd=hle::libc::FdTable::get().insert(file);
  auto extractor=static_cast<GuestAddr>(call("AMediaExtractor_new"));REQUIRE(extractor);
  CHECK(call("AMediaExtractor_setDataSourceFd",{extractor,static_cast<std::uint64_t>(fd),31,file->bytes.size()-67})==media::unsupported);
  CHECK(call("AMediaExtractor_getTrackCount",{extractor})==0);
  CHECK(call("AMediaExtractor_delete",{extractor})==0);
  hle::libc::FdTable::get().close(fd);
}
TEST_CASE("OpenSL file descriptor and URI players use decoded PCM seek and loop") {
  namespace sl=hle::opensles::abi;
  audio::register_libraries();
  REQUIRE(manual_audio_device());
  auto invoke=[](GuestAddr fn,std::initializer_list<std::uint64_t> args) {
    return test::audio_core::call(fn,args);
  };
  auto object=[](GuestAddr p)->const sl::ObjectTable& {return *g2h<sl::ObjectTable>(*g2h<GuestAddr>(p));};
  auto interface=[&](GuestAddr p,const char* name) {
    auto id=*g2h<GuestAddr>(test::audio_core::symbol("libOpenSLES.so",name));
    GuestAddr result=0;REQUIRE(invoke(object(p).GetInterface,{p,id,h2g(&result)})==0);return result;
  };
  GuestAddr engine=0,mix=0;
  REQUIRE(invoke(test::audio_core::symbol("libOpenSLES.so","slCreateEngine"),{h2g(&engine),0,0,0,0,0})==0);
  REQUIRE(invoke(object(engine).Realize,{engine,0})==0);
  auto ei=interface(engine,"SL_IID_ENGINE");auto& et=*g2h<sl::EngineTable>(*g2h<GuestAddr>(ei));
  REQUIRE(invoke(et.CreateOutputMix,{ei,h2g(&mix),0,0,0})==0);
  REQUIRE(invoke(object(mix).Realize,{mix,0})==0);
  sl::OutputMixLocator output{4,0,mix};sl::Data sink{h2g(&output),0};
  std::uint32_t mime=1;
  auto file=std::make_shared<RegionFile>();auto fd=hle::libc::FdTable::get().insert(file);
  sl::FdLocator locator{0x800007bc,fd,31,static_cast<std::int64_t>(file->bytes.size()-67)};
  sl::Data source{h2g(&locator),h2g(&mime)};GuestAddr player=0;
  REQUIRE(invoke(et.CreateAudioPlayer,{ei,h2g(&player),h2g(&source),h2g(&sink),0,0,0})==0);
  REQUIRE(invoke(object(player).Realize,{player,0})==0);
  auto play=interface(player,"SL_IID_PLAY"),seek=interface(player,"SL_IID_SEEK");
  auto& pt=*g2h<sl::PlayTable>(*g2h<GuestAddr>(play));auto& st=*g2h<sl::SeekTable>(*g2h<GuestAddr>(seek));
  std::uint32_t duration=0;REQUIRE(invoke(pt.GetDuration,{play,h2g(&duration)})==0);CHECK(duration==250);
  REQUIRE(invoke(st.SetLoop,{seek,1,0,UINT32_MAX})==0);
  REQUIRE(invoke(pt.SetPlayState,{play,3})==0);
  for(int i=0;i<65;++i)pump();
  std::uint32_t state=0;REQUIRE(invoke(pt.GetPlayState,{play,h2g(&state)})==0);CHECK(state==3);
  REQUIRE(invoke(st.SetPosition,{seek,80,2})==0);
  REQUIRE(invoke(st.SetLoop,{seek,0,0,UINT32_MAX})==0);
  for(int i=0;i<65;++i)pump();
  REQUIRE(invoke(pt.GetPlayState,{play,h2g(&state)})==0);CHECK(state==1);
  CHECK(file->sequential==17);invoke(object(player).Destroy,{player});hle::libc::FdTable::get().close(fd);
  // Mount only this authored fixture as the test's APK alias. URI resolution
  // still goes through the actual HLE VFS, never FFmpeg's file protocol.
  auto& config=process_config();auto previous_apk=config.apk;auto previous_package=config.package;
  config.package="com.venture.media.test";config.apk=VENTURE_MEDIA_FIXTURE;
  auto uri="file://"+config.guest_apk_path();sl::UriLocator url{1,0,h2g(uri.c_str())};
  source={h2g(&url),h2g(&mime)};player=0;
  REQUIRE(invoke(et.CreateAudioPlayer,{ei,h2g(&player),h2g(&source),h2g(&sink),0,0,0})==0);
  REQUIRE(invoke(object(player).Realize,{player,0})==0);
  play=interface(player,"SL_IID_PLAY");auto& up=*g2h<sl::PlayTable>(*g2h<GuestAddr>(play));
  REQUIRE(invoke(up.SetPlayState,{play,3})==0);
  for(int i=0;i<65;++i)pump();
  REQUIRE(invoke(up.GetPlayState,{play,h2g(&state)})==0);CHECK(state==1);
  CHECK(audio::Backend::get().mixer_stats().non_silent_samples>0);
  invoke(object(engine).Destroy,{engine});config.apk=previous_apk;config.package=previous_package;
}
TEST_CASE("source-triggered host capture uses the actual decoded media stream" * doctest::skip()) {
  REQUIRE(audio::Backend::get().use_null_device());
  auto file=std::make_shared<RegionFile>();media::Player player;
  REQUIRE(player.source(input(file))==0);REQUIRE(player.prepare()==0);
  player.loop(true);REQUIRE(player.start()==0);
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  CHECK(player.stream()->non_silent_written>0);
  CHECK(player.stream()->frames_read>0);
  player.release();
}

namespace {
media::Input video_input(const char* path=VENTURE_VIDEO_FIXTURE) {
  std::ifstream file(path,std::ios::binary);
  auto bytes=std::make_shared<std::vector<char>>(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>());
  return {static_cast<std::int64_t>(bytes->size()),[bytes](void* out,std::uint64_t size,std::int64_t offset)->std::int64_t {
    if(offset<0 || std::uint64_t(offset)>bytes->size())return -1;
    size=std::min<std::uint64_t>(size,bytes->size()-offset);std::memcpy(out,bytes->data()+offset,size);return size;
  }};
}
}
TEST_CASE("video decoder preserves B-frame presentation times and converts padded YUV rows") {
  bool scale=false;
  SUBCASE("source dimensions"){scale=false;}
  SUBCASE("fixed Surface dimensions and resize"){scale=true;}
  media::Extractor extractor;REQUIRE(extractor.open(video_input())==0);
  REQUIRE(extractor.tracks().size()==1);auto track=extractor.tracks()[0];
  CHECK(track.mime=="video/avc");REQUIRE(extractor.select(0)==0);
  media::Decoder decoder;REQUIRE(decoder.configure(track)==0);
  int count=0;std::int64_t previous=-1;bool drain=false;
  std::uint64_t first=0,last=0;
  for(int budget=0;budget<100;++budget) {
    auto width=scale?(count<6?32:16):128,height=scale?(count<6?18:9):72;
    media::VideoFrame frame;auto result=decoder.receive(&frame,std::numeric_limits<std::int64_t>::min(),width,height);
    if(result==-1)break;REQUIRE(result>=0);
    if(result==1) {
      REQUIRE(frame.width==width);REQUIRE(frame.height==height);REQUIRE(frame.rgba.size()==std::size_t(width*height*4));
      CHECK(frame.time_us>previous);previous=frame.time_us;
      std::uint64_t sum=0;for(std::size_t i=0;i<frame.rgba.size();i+=4){sum+=frame.rgba[i];CHECK(frame.rgba[i+3]==255);}
      if(!count)first=sum;last=sum;++count;
    } else if(extractor.sample().track>=0) {
      REQUIRE(decoder.send(&extractor.sample())==0);extractor.advance();
    } else {REQUIRE_FALSE(drain);REQUIRE(decoder.send(nullptr)==0);drain=true;}
  }
  CHECK(count==12);CHECK(previous==916667);CHECK(first!=last);
  decoder.flush();REQUIRE(extractor.seek(0,0)==0);REQUIRE(decoder.send(&extractor.sample())==0);
}
TEST_CASE("bounded video codec owns buffers across flush EOS surface change and release") {
  media::Extractor extractor;REQUIRE(extractor.open(video_input())==0);REQUIRE(extractor.select(0)==0);
  media::Codec codec;media::Codec::Info info;
  CHECK(codec.start()==media::bad_state);CHECK(codec.flush()==media::bad_state);
  CHECK(codec.input(0)==media::bad_state);
  std::atomic<int> delivered{0},replacement{0};std::atomic<bool> off_thread{false};
  auto caller=std::this_thread::get_id();
  REQUIRE(codec.configure(extractor.tracks()[0],[&](const media::VideoFrame& frame) {
    CHECK(frame.rgba.size()==128*72*4);off_thread=std::this_thread::get_id()!=caller;++delivered;return 0;
  })==0);
  REQUIRE(codec.start()==0);CHECK(codec.format().color_format==media::color_surface);CHECK(codec.start()==media::bad_state);
  CHECK(codec.configure(extractor.tracks()[0])==media::bad_state);
  REQUIRE(codec.input(0)==0);CHECK(codec.input(0)==-1);
  CHECK_FALSE(codec.input_buffer(0).empty());REQUIRE(codec.flush()==0);
  CHECK(codec.input_buffer(0).empty());CHECK(codec.queue(0,0,1,0,0)==media::bad_state);
  std::int64_t owned=-1;
  auto first_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
  // Reordering and frame threading may need more than four compressed
  // packets before the first picture. Keep driving both sides of the API.
  while(owned<0 && std::chrono::steady_clock::now()<first_deadline) {
    if(extractor.sample().track>=0 && codec.input(1000)==0) {
      auto buffer=codec.input_buffer(0);auto packet=extractor.sample();
      std::copy(packet.bytes.begin(),packet.bytes.end(),buffer.begin());
      REQUIRE(codec.queue(0,0,packet.bytes.size(),packet.time_us,0)==0);extractor.advance();
    }
    owned=codec.output(&info,1000);
  }
  REQUIRE(owned==0);CHECK(codec.output_buffer(0).empty());REQUIRE(codec.flush()==0);CHECK(codec.release(0,true)==media::invalid);
  REQUIRE(extractor.seek(0,0)==0);
  int frames=0;bool sent_eos=false,got_eos=false;
  auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(!got_eos && std::chrono::steady_clock::now()<deadline) {
    auto input_index=sent_eos?-1:codec.input(1000);
    if(input_index==0) {
      auto& packet=extractor.sample();auto buffer=codec.input_buffer(0);
      if(packet.track>=0) {
        REQUIRE(packet.bytes.size()<=buffer.size());std::copy(packet.bytes.begin(),packet.bytes.end(),buffer.begin());
        REQUIRE(codec.queue(0,0,packet.bytes.size(),packet.time_us,0)==0);extractor.advance();
      } else {REQUIRE(codec.queue(0,0,0,1000000,4)==0);sent_eos=true;}
    }
    auto index=codec.output(&info,1000);if(index==-2 || index==-1)continue;REQUIRE(index==0);
    got_eos=(info.flags&4)!=0;
    if(!got_eos) {
      ++frames;REQUIRE(info.size==128*72*4);
      if(frames==6)REQUIRE(codec.surface([&](const media::VideoFrame&){++replacement;return 0;})==0);
    }
    REQUIRE(codec.release(0,true)==0);CHECK(codec.release(0,false)==media::invalid);
  }
  CHECK(got_eos);CHECK(frames==12);
  while(replacement<6 && std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  CHECK(off_thread);CHECK(replacement>=6);
  REQUIRE(codec.flush()==0);REQUIRE(codec.input(10000)==0);
  REQUIRE(codec.stop()==0);CHECK(codec.output(&info,0)==media::bad_state);
  REQUIRE(codec.start()==0);REQUIRE(codec.input(0)==0);
  // Destruction with an outstanding input buffer joins the worker.
}

TEST_CASE("codec release wakes blocked dequeues retained by concurrent API calls") {
  media::Extractor extractor;REQUIRE(extractor.open(video_input())==0);
  auto codec=std::make_shared<media::Codec>();REQUIRE(codec->configure(extractor.tracks()[0])==0);
  REQUIRE(codec->start()==0);REQUIRE(codec->input(0)==0);
  media::Codec::Info info;REQUIRE(codec->output(&info,0)==-2);
  auto input=std::async(std::launch::async,[codec]{return codec->input(-1);});
  auto output=std::async(std::launch::async,[codec]{media::Codec::Info next;return codec->output(&next,-1);});
  CHECK(input.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);
  CHECK(output.wait_for(std::chrono::milliseconds(20))==std::future_status::timeout);
  codec->release();codec->release();
  REQUIRE(input.wait_for(std::chrono::seconds(2))==std::future_status::ready);
  REQUIRE(output.wait_for(std::chrono::seconds(2))==std::future_status::ready);
  CHECK(input.get()==media::bad_state);CHECK(output.get()==media::bad_state);
  CHECK(codec->start()==media::bad_state);CHECK(codec->configure(extractor.tracks()[0])==media::bad_state);
}

TEST_CASE("video-only player follows consumed audio time through pause seek loop and teardown") {
  REQUIRE(manual_audio_device());media::Player player;std::atomic<int> frames{0},wraps{0};
  std::atomic<std::int64_t> required_floor{0};
  std::int64_t previous=-1;
  player.surface([&](const media::VideoFrame& frame){CHECK(frame.time_us>=required_floor.load());if(frame.time_us<previous)++wraps;previous=frame.time_us;++frames;return 0;});
  REQUIRE(player.source(video_input())==0);REQUIRE(player.prepare()==0);
  CHECK(player.video_width()==128);CHECK(player.video_height()==72);CHECK(player.duration_us()==1000000);
  player.loop(true);REQUIRE(player.start()==0);
  auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(wraps==0 && std::chrono::steady_clock::now()<deadline)pump();
  CHECK(frames>12);CHECK(wraps>0);REQUIRE(player.pause()==0);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));auto paused=frames.load();auto position=player.position_us();
  for(int i=0;i<8;++i)pump();CHECK(frames==paused);CHECK(player.position_us()==position);
  REQUIRE(player.seek(700000)==0);CHECK(player.position_us()==700000);
  required_floor=700000;player.loop(false);REQUIRE(player.start()==0);
  // This is a new playback interval. Time spent establishing the first loop
  // and pausing must not consume the seek/EOS phase's pumping budget.
  deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(!player.completed() && std::chrono::steady_clock::now()<deadline)pump();
  CHECK(player.completed());CHECK(player.position_us()==1000000);CHECK(player.error()==0);
  required_floor=0;REQUIRE(player.start()==0);player.release();player.release();CHECK_FALSE(player.playing());
}

TEST_CASE("AV player keeps video on the consumption clock after its shorter audio track ends") {
  REQUIRE(manual_audio_device());media::Player player;
  std::atomic<int> frames{0};std::atomic<std::int64_t> last_pts{-1},ahead_us{0};
  player.surface([&](const media::VideoFrame& frame) {
    ahead_us=std::max(ahead_us.load(),frame.time_us-player.position_us());
    last_pts=frame.time_us;++frames;return 0;
  });
  REQUIRE(player.source(video_input(VENTURE_AV_FIXTURE))==0);REQUIRE(player.prepare()==0);
  REQUIRE(player.duration_us()==1000000);REQUIRE(player.start()==0);
  auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(!player.completed() && std::chrono::steady_clock::now()<deadline)pump();
  CHECK(player.completed());CHECK(player.error()==0);
  CHECK(player.stream()->non_silent_written>0);CHECK(player.position_us()==1000000);
  CHECK(frames>=9);CHECK(last_pts>=750000);CHECK(ahead_us<=0);
  auto stream=player.stream();auto consumed=stream->frames_read.load(),written=stream->frames_written.load();
  auto non_silent=stream->non_silent_written.load();
  player.loop(true);REQUIRE(player.start()==0);
  // The manual device advances 5 ms per pump, independent of host scheduling.
  // Background build admission can stretch each sleep beyond its request.
  deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
  while(stream->frames_read-consumed<72000 && std::chrono::steady_clock::now()<deadline)pump();
  REQUIRE(stream->frames_read-consumed>=72000);
  // Two short tones belong to two full one-second AV cycles. Looping the
  // audio track every quarter second would instead make this mostly sound.
  CHECK(stream->non_silent_written-non_silent>0);
  CHECK(stream->non_silent_written-non_silent<std::uint64_t(stream->frames_written-written));
  CHECK(player.error()==0);
  auto loop_position=player.position_us();player.loop(false);CHECK(player.position_us()==loop_position);
  player.release();
}

TEST_CASE("AV player drops late pictures without stopping its audio consumption clock") {
  REQUIRE(manual_audio_device());std::atomic<int> frames{0};std::atomic<std::int64_t> maximum_lag{0};
  media::Player player;
  player.surface([&](const media::VideoFrame& frame) {
    maximum_lag=std::max(maximum_lag.load(),player.position_us()-frame.time_us);++frames;
    std::this_thread::sleep_for(std::chrono::milliseconds(180));return 0;
  });
  REQUIRE(player.source(video_input(VENTURE_AV_FIXTURE))==0);REQUIRE(player.prepare()==0);REQUIRE(player.start()==0);
  auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
  while(!player.completed() && std::chrono::steady_clock::now()<deadline)pump();
  CHECK(player.completed());CHECK(player.error()==0);CHECK(player.position_us()==1000000);
  player.release();CHECK(frames>=4);
  // One manual-device quantum may elapse between scheduler and observation.
  CHECK(maximum_lag<=55000);
}

TEST_CASE("WebM VP9 uses container duration and outputs ordered RGBA frames") {
  media::Extractor extractor;REQUIRE(extractor.open(video_input(VENTURE_VP9_FIXTURE))==0);
  REQUIRE(extractor.tracks().size()==1);auto track=extractor.tracks()[0];
  CHECK(track.mime=="video/x-vnd.on2.vp9");CHECK(track.duration_us==1000000);
  REQUIRE(extractor.select(0)==0);media::Decoder decoder;REQUIRE(decoder.configure(track)==0);
  int frames=0;std::int64_t previous=-1;bool draining=false,eos=false;
  for(int budget=0;budget<100;++budget) {
    media::VideoFrame frame;auto result=decoder.receive(&frame);
    if(result==-1){eos=true;break;}REQUIRE(result>=0);
    if(result==1){CHECK(frame.rgba.size()==128*72*4);CHECK(frame.time_us>previous);previous=frame.time_us;++frames;}
    else if(extractor.sample().track>=0){REQUIRE(decoder.send(&extractor.sample())==0);extractor.advance();}
    else {REQUIRE_FALSE(draining);REQUIRE(decoder.send(nullptr)==0);draining=true;}
  }
  CHECK(frames==12);CHECK(eos);
  media::Player player;REQUIRE(player.source(video_input(VENTURE_VP9_FIXTURE))==0);
  REQUIRE(player.prepare()==0);CHECK(player.duration_us()==1000000);REQUIRE(player.seek(500000)==0);player.release();
}
TEST_CASE("native-window frame notification unlocks before callback and preserves explicit zero PTS") {
  auto window=android::create_native_window(16,8);REQUIRE(window);
  std::atomic<int> notifications{0};
  {std::lock_guard lock(window->lock);window->image_consumer=true;window->frame_available=[&]{std::lock_guard lock(window->lock);++notifications;};}
  android::WindowBuffer buffer;REQUIRE(android::lock_window(window,&buffer,nullptr)==0);
  std::memset(g2h(buffer.bits),255,16*8*4);REQUIRE(android::unlock_window(window)==0);
  CHECK(notifications==1);CHECK(window->frame->timestamp_ns>0);
  auto pixels=window->frame->rgba;
  REQUIRE(android::publish_window_rgba(window,pixels,16,8,false,0)==0);
  CHECK(notifications==2);CHECK(window->frame->timestamp_ns==0);
  REQUIRE(android::publish_window_rgba(window,pixels,16,8)==0);
  CHECK(notifications==3);CHECK(window->frame->timestamp_ns>0);
  {std::lock_guard lock(window->lock);window->frame_available={};}
  android::release(window);
}
TEST_CASE("AImageReader queues window frames as CPU images in RGBA and YUV_420_888") {
  GuestAddr reader=0;
  CHECK(call("AImageReader_new",{4,2,0x100,2,h2g(&reader)})==-10002);CHECK(reader==0);  // JPEG
  REQUIRE(call("AImageReader_new",{4,2,1,2,h2g(&reader)})==0);REQUIRE(reader);
  std::int32_t value=0;REQUIRE(call("AImageReader_getMaxImages",{reader,h2g(&value)})==0);CHECK(value==2);
  GuestAddr window=0;REQUIRE(call("AImageReader_getWindow",{reader,h2g(&window)})==0);
  auto* native=android::native_window_from_guest(window);REQUIRE(native);
  GuestAddr image=0;CHECK(call("AImageReader_acquireNextImage",{reader,h2g(&image)})==-30001);
  std::vector<std::uint8_t> pixels(4*2*4);
  for(std::uint8_t frame=1;frame<=3;++frame) {
    std::fill(pixels.begin(),pixels.end(),frame);
    REQUIRE(android::publish_window_rgba(native,pixels,4,2,false,frame*1000)==0);
  }
  // Two frames fit the queue; the latest drops the rest.
  REQUIRE(call("AImageReader_acquireLatestImage",{reader,h2g(&image)})==0);REQUIRE(image);
  std::int64_t time=0;REQUIRE(call("AImage_getTimestamp",{image,h2g(&time)})==0);CHECK(time==3000);
  REQUIRE(call("AImage_getNumberOfPlanes",{image,h2g(&value)})==0);CHECK(value==1);
  REQUIRE(call("AImage_getPlaneRowStride",{image,0,h2g(&value)})==0);CHECK(value==16);
  GuestAddr data=0;std::int32_t length=0;
  REQUIRE(call("AImage_getPlaneData",{image,0,h2g(&data),h2g(&length)})==0);
  REQUIRE(length==32);CHECK(g2h<std::uint8_t>(data)[31]==3);
  GuestAddr none=0;CHECK(call("AImageReader_acquireNextImage",{reader,h2g(&none)})==-30001);
  for(int n=0;n<2;++n)REQUIRE(android::publish_window_rgba(native,pixels,4,2)==0);
  GuestAddr second=0,third=0;
  REQUIRE(call("AImageReader_acquireNextImage",{reader,h2g(&second)})==0);
  CHECK(call("AImageReader_acquireNextImage",{reader,h2g(&third)})==-30002);  // maxImages acquired
  call("AImage_delete",{image});
  REQUIRE(call("AImageReader_acquireNextImage",{reader,h2g(&third)})==0);
  call("AImage_delete",{second});call("AImage_delete",{third});
  call("AImageReader_delete",{reader});
  CHECK(call("AImageReader_getWindow",{reader,h2g(&window)})==-10003);

  REQUIRE(call("AImageReader_new",{2,2,0x23,1,h2g(&reader)})==0);
  REQUIRE(call("AImageReader_getWindow",{reader,h2g(&window)})==0);
  pixels.assign(2*2*4,0);
  for(int n=0;n<4;++n){pixels[n*4]=255;pixels[n*4+3]=255;}  // red
  REQUIRE(android::publish_window_rgba(android::native_window_from_guest(window),pixels,2,2)==0);
  REQUIRE(call("AImageReader_acquireNextImage",{reader,h2g(&image)})==0);
  REQUIRE(call("AImage_getNumberOfPlanes",{image,h2g(&value)})==0);CHECK(value==3);
  REQUIRE(call("AImage_getPlaneData",{image,0,h2g(&data),h2g(&length)})==0);CHECK(length==4);
  CHECK(g2h<std::uint8_t>(data)[0]==82);  // BT.601 limited-range red
  REQUIRE(call("AImage_getPlaneData",{image,2,h2g(&data),h2g(&length)})==0);CHECK(length==1);
  CHECK(g2h<std::uint8_t>(data)[0]==240);
  call("AImage_delete",{image});call("AImageReader_delete",{reader});
}
TEST_CASE("AImageReader announces frames to its listener on its own thread") {
  GuestAddr reader=0;REQUIRE(call("AImageReader_new",{2,2,1,2,h2g(&reader)})==0);
  std::atomic<int> calls{0};std::atomic<GuestAddr> seen{0};
  auto callback=test::android_callback([&](const GuestCallArgs& args){
    calls+=int(args.integers[0]);seen=args.integers[1];return GuestCallResult{};});
  GuestAddr listener[2]{7,callback};
  REQUIRE(call("AImageReader_setImageListener",{reader,h2g(listener)})==0);
  GuestAddr window=0;REQUIRE(call("AImageReader_getWindow",{reader,h2g(&window)})==0);
  std::vector<std::uint8_t> pixels(2*2*4,9);
  REQUIRE(android::publish_window_rgba(android::native_window_from_guest(window),pixels,2,2)==0);
  REQUIRE(test::wait_until([&]{return calls.load()==7;}));CHECK(seen==reader);
  call("AImageReader_delete",{reader});test::android_remove_callback(callback);
}
