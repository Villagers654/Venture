// Offline verification of the same bounded MOV/AAC path used by the players.
// Inputs and decoded outputs stay in the caller's owned evidence directory.
#include "media/media.h"
#include "gfx/shared_image.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <atomic>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <limits>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/resource.h>
#endif

namespace {
using clock_type=std::chrono::steady_clock;
double seconds_since(clock_type::time_point t){return std::chrono::duration<double>(clock_type::now()-t).count();}
std::int64_t now_ns(){return std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now().time_since_epoch()).count();}
// Process CPU time (user + kernel, all threads) in seconds.
double cpu_seconds() {
#ifdef _WIN32
  FILETIME created,exited,kernel,user;
  if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user))return 0;
  auto value=[](FILETIME t){return (std::uint64_t(t.dwHighDateTime)<<32|t.dwLowDateTime)/1e7;};
  return value(kernel)+value(user);
#else
  rusage usage{};getrusage(RUSAGE_SELF,&usage);
  return usage.ru_utime.tv_sec+usage.ru_utime.tv_usec/1e6+usage.ru_stime.tv_sec+usage.ru_stime.tv_usec/1e6;
#endif
}
venture::media::Input open_input(const char* path) {
  auto file=std::make_shared<std::ifstream>(path,std::ios::binary);
  file->seekg(0,std::ios::end);auto length=std::int64_t(file->tellg());
  venture::media::Input input;
  if(!*file || length<=0 || length>512LL*1024*1024)return input;
  input.length=length;
  input.read_at=[file,length](void* data,std::uint64_t size,std::int64_t offset)->std::int64_t {
    if(offset<0 || offset>length)return -1;
    size=std::min<std::uint64_t>(size,length-offset);
    file->clear();file->seekg(offset);file->read(static_cast<char*>(data),size);
    return file->bad()?-1:file->gcount();
  };
  return input;
}
int video_track(venture::media::Extractor& extractor) {
  for(std::size_t i=0;i<extractor.tracks().size();++i)
    if(venture::media::video_mime(extractor.tracks()[i].mime))return extractor.select(int(i))?-1:int(i);
  return -1;
}
// The Surface codec released on presentation deadlines as ExoPlayer does (a
// frame more than 30 ms late is dropped, others render at their time), to a
// CPU consumer or to a consumer importing GPU pictures.
int surface_paced(const char* path,std::ostream& report,int width,int height,bool shared) {
  using namespace venture::media;
  Extractor extractor;if(extractor.open(open_input(path)))return 3;
  int track=video_track(extractor);if(track<0)return 3;
  std::atomic<std::uint64_t> presented{0},gpu_pictures{0};std::atomic<std::int64_t> present_late_ns{0},present_late_max_ns{0};
  std::atomic<std::int64_t> start{0};
  Codec codec;
  if(codec.configure(extractor.tracks()[track],[&](const VideoFrame& frame) {
    auto late=now_ns()-(start.load()+frame.time_us*1000);
    present_late_ns+=late;if(late>present_late_max_ns)present_late_max_ns=late;
    if(frame.image)++gpu_pictures;
    ++presented;return 0;
  },[width,height,shared]{return Codec::Target({width,height},shared);}) || codec.start())return 3;
  bool sent=false,eos=false;std::uint64_t rendered=0,dropped=0,outputs=0;
  auto deadline=clock_type::now()+std::chrono::seconds(180);
  auto begin=clock_type::now();auto cpu_begin=cpu_seconds();
  while(!eos && clock_type::now()<deadline) {
    if(!sent && codec.input(0)==0) {
      auto& packet=extractor.sample();
      if(packet.track>=0) {
        auto buffer=codec.input_buffer(0);if(packet.bytes.size()>buffer.size())return 4;
        std::copy(packet.bytes.begin(),packet.bytes.end(),buffer.begin());
        if(codec.queue(0,0,packet.bytes.size(),packet.time_us,0))return 4;
        extractor.advance();if(extractor.error())return 4;
      } else {if(codec.queue(0,0,0,0,4))return 4;sent=true;}
    }
    Codec::Info info;auto index=codec.output(&info,1000);
    if(index==-1 || index==-2)continue;if(index<0)return 4;
    eos=info.flags&4;if(eos){codec.release(index,false);break;}
    ++outputs;
    // The clock starts with the first decoded picture, as playback starts
    // once the first frame is ready.
    if(!start)start=now_ns()-info.time*1000;
    auto due=start+info.time*1000;
    if(now_ns()>due+30000000){++dropped;if(codec.release(index,false))return 4;continue;}
    while(now_ns()<due-50000000)std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ++rendered;if(codec.release(index,true,due))return 4;
  }
  auto elapsed=seconds_since(begin);auto cpu=cpu_seconds()-cpu_begin;
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  codec.release();
  report<<"{\"phase\":\""<<(shared?"surface_shared":"surface_paced")<<"\",\"outputs\":"<<outputs<<",\"rendered\":"<<rendered
    <<",\"dropped\":"<<dropped<<",\"presented\":"<<presented<<",\"gpu_pictures\":"<<gpu_pictures<<",\"seconds\":"<<elapsed
    <<",\"cpu_seconds\":"<<cpu<<",\"present_late_avg_ms\":"<<(presented?present_late_ns/1e6/presented:0)
    <<",\"present_late_max_ms\":"<<present_late_max_ns/1e6<<",\"eos\":"<<eos<<"}\n";
  return eos?0:5;
}
// Movie throughput on this machine: raw decode, conversion at a consumer size,
// and paced Surface playback.
int video_rate(const char* path,std::ostream& report,int width,int height) {
  using namespace venture::media;
  std::uint64_t frames=0;double decode_s=0,decode_cpu=0,convert_ms=0,convert_max_ms=0;std::uint64_t converted=0;
  std::int64_t last_pts=0;std::string backend;
  for(int pass=0;pass<2;++pass) {
    Extractor extractor;if(extractor.open(open_input(path)))return 3;
    int track=video_track(extractor);if(track<0)return 3;
    Decoder decoder;if(decoder.configure(extractor.tracks()[track]))return 3;
    auto begin=clock_type::now();auto cpu_begin=cpu_seconds();bool draining=false;std::uint64_t count=0;
    for(;;) {
      VideoFrame frame;
      // Pass 0 decodes without download or conversion; pass 1 converts
      // every picture to RGBA at the consumer size.
      auto status=pass==0?decoder.receive(&frame,std::numeric_limits<std::int64_t>::max()):decoder.receive(&frame,std::numeric_limits<std::int64_t>::min(),width,height);
      if(status==-1)break;if(status<0)return 4;
      if(status==1 || status==2) {
        ++count;last_pts=std::max(last_pts,frame.time_us);
        if(status==1){auto ms=decoder.video_conversion_us()/1000.0;convert_ms+=ms;convert_max_ms=std::max(convert_max_ms,ms);++converted;}
        continue;
      }
      if(draining)return 4;
      if(extractor.sample().track>=0) {
        if(decoder.send(&extractor.sample()))return 4;
        extractor.advance();if(extractor.error())return 4;
      } else {if(decoder.send(nullptr))return 4;draining=true;}
    }
    if(pass==0){frames=count;decode_s=seconds_since(begin);decode_cpu=cpu_seconds()-cpu_begin;backend=decoder.video_backend();}
    else report<<"{\"phase\":\"decode_convert\",\"frames\":"<<count<<",\"seconds\":"<<seconds_since(begin)
      <<",\"cpu_seconds\":"<<cpu_seconds()-cpu_begin
      <<",\"convert_avg_ms\":"<<(converted?convert_ms/converted:0)<<",\"convert_max_ms\":"<<convert_max_ms<<"}\n";
  }
  auto movie_s=frames>1?last_pts/1e6*frames/(frames-1):0.0;
  report<<"{\"phase\":\"decode_only\",\"backend\":\""<<backend<<"\",\"frames\":"<<frames<<",\"seconds\":"<<decode_s
    <<",\"cpu_seconds\":"<<decode_cpu<<",\"fps\":"<<frames/decode_s
    <<",\"movie_seconds\":"<<movie_s<<",\"movie_fps\":"<<(movie_s>0?frames/movie_s:0)<<"}\n";
  if(auto result=surface_paced(path,report,width,height,false))return result;
  return surface_paced(path,report,width,height,true);
}
}

// Writes picture `index` converted to RGBA at width x height, to compare
// conversion paths (VENTURE_MEDIA_SOFTWARE_VIDEO=1 selects the CPU path).
int dump_frame(const char* path,const char* output,int index,int width,int height) {
  using namespace venture::media;
  Extractor extractor;if(extractor.open(open_input(path)))return 3;
  int track=video_track(extractor);if(track<0)return 3;
  Decoder decoder;if(decoder.configure(extractor.tracks()[track]))return 3;
  bool draining=false;int count=0;
  for(;;) {
    VideoFrame frame;
    auto status=decoder.receive(&frame,std::numeric_limits<std::int64_t>::min(),width,height);
    if(status==-1)return 4;if(status<0)return 4;
    if(status==1) {
      if(count++<index)continue;
      std::ofstream out(output,std::ios::binary|std::ios::trunc);
      out.write(reinterpret_cast<const char*>(frame.rgba.data()),std::streamsize(frame.rgba.size()));
      std::cout<<"{\"backend\":\""<<decoder.video_backend()<<"\",\"pts_us\":"<<frame.time_us<<",\"width\":"<<frame.width
        <<",\"height\":"<<frame.height<<"}\n";
      return out?0:2;
    }
    if(draining)return 4;
    if(extractor.sample().track>=0) {
      if(decoder.send(&extractor.sample()))return 4;
      extractor.advance();if(extractor.error())return 4;
    } else {if(decoder.send(nullptr))return 4;draining=true;}
  }
}

int main(int argc, char** argv) {
  if(argc==7 && std::string_view(argv[3])=="--dump-frame")
    return dump_frame(argv[1],argv[2],std::atoi(argv[4]),std::atoi(argv[5]),std::atoi(argv[6]));
  if(argc>=4 && std::string_view(argv[3])=="--video-rate") {
    int width=argc>=6?std::atoi(argv[4]):0,height=argc>=6?std::atoi(argv[5]):0;
    std::ofstream report(argv[2],std::ios::trunc);if(!report)return 2;
    auto result=video_rate(argv[1],report,width,height);
    report.close();std::ifstream echo(argv[2]);std::cout<<echo.rdbuf();return result;
  }
  if (argc != 3 && argc != 4) return 2;
  const bool video=argc==4 && std::string_view(argv[3])=="--video";
  if(argc==4 && !video)return 2;
  auto file = std::make_shared<std::ifstream>(argv[1], std::ios::binary);
  file->seekg(0, std::ios::end);
  auto length = file->tellg();
  if (!*file || length <= 0 || length > 512LL * 1024 * 1024) return 2;
  venture::media::Input input;
  input.length = length;
  input.read_at = [file, length](void* data, std::uint64_t size, std::int64_t offset) -> std::int64_t {
    if (offset < 0 || offset > length) return -1;
    size = std::min<std::uint64_t>(size, std::int64_t(length) - offset);
    file->clear(); file->seekg(offset); file->read(static_cast<char*>(data), size);
    return file->bad() ? -1 : file->gcount();
  };
  venture::media::Extractor extractor;
  if (extractor.open(std::move(input))) return 3;
  if(video) {
    int selected=-1;
    for(std::size_t i=0;i<extractor.tracks().size();++i)
      if(venture::media::video_mime(extractor.tracks()[i].mime)){selected=static_cast<int>(i);break;}
    if(selected<0 || extractor.select(selected))return 3;
    std::ofstream output(argv[2],std::ios::trunc);if(!output)return 2;
    std::atomic<std::uint64_t> presented{0};venture::media::Codec codec;
    if(codec.configure(extractor.tracks()[selected],[&](const venture::media::VideoFrame& frame) {
      output<<"{\"pts_us\":"<<frame.time_us<<",\"width\":"<<frame.width<<",\"height\":"<<frame.height<<"}\n";
      ++presented;return output?0:-1;
    }) || codec.start())return 3;
    bool sent=false,eos=false;std::uint64_t decoded=0;
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    while(!eos && std::chrono::steady_clock::now()<deadline) {
      if(!sent && codec.input(1000)==0) {
        auto& packet=extractor.sample();
        if(packet.track>=0) {
          auto buffer=codec.input_buffer(0);if(packet.bytes.size()>buffer.size())return 4;
          std::copy(packet.bytes.begin(),packet.bytes.end(),buffer.begin());
          if(codec.queue(0,0,packet.bytes.size(),packet.time_us,0))return 4;
          extractor.advance();if(extractor.error())return 4;
        } else {if(codec.queue(0,0,0,0,4))return 4;sent=true;}
      }
      venture::media::Codec::Info info;auto index=codec.output(&info,1000);
      if(index==-1 || index==-2)continue;if(index<0)return 4;
      eos=info.flags&4;if(!eos)++decoded;
      if(codec.release(index,true))return 4;
      // Exercise Surface release without dropping frames in this offline
      // integrity probe. This wait is not a playback-performance measurement.
      while(presented<decoded && std::chrono::steady_clock::now()<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    codec.release();output.close();if(!eos || presented!=decoded || !output)return 4;
    std::cout<<"{\"source_sha256\":\""<<extractor.source_hash()<<"\",\"video_frames\":"<<decoded<<"}\n";
    return 0;
  }
  int index = -1;
  for (std::size_t i = 0; i < extractor.tracks().size(); ++i)
    if (extractor.tracks()[i].mime == "audio/mp4a-latm") { index = int(i); break; }
  if (index < 0) return 3;
  const auto& track = extractor.tracks()[index];
  venture::media::Decoder decoder;
  if (decoder.configure(track) || extractor.select(index)) return 3;
  std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
  if (!output) return 2;
  bool draining = false;
  std::uint64_t frames = 0;
  for (;;) {
    venture::media::Pcm pcm;
    auto status = decoder.receive(&pcm);
    if (status == -1) break;
    if (status < 0) return 4;
    if (status == 1) {
      if (pcm.rate != track.rate || pcm.channels != track.channels) return 4;
      // Match Player's origin/duration clipping, including container padding.
      venture::media::trim_pcm(pcm,0,track.duration_us);
      output.write(reinterpret_cast<const char*>(pcm.samples.data()),pcm.samples.size()*sizeof(float));
      frames += pcm.samples.size()/pcm.channels;
    } else {
      if (draining) return 4;
      if (extractor.sample().track >= 0) {
        if (decoder.send(&extractor.sample())) return 4;
        extractor.advance();
        if (extractor.error()) return 4;
      } else {
        if (decoder.send(nullptr)) return 4;
        draining = true;
      }
    }
  }
  output.close();
  if (!output) return 4;
  std::cout << "{\"source_sha256\":\"" << extractor.source_hash()
            << "\",\"frames\":" << frames << ",\"rate\":" << track.rate
            << ",\"channels\":" << track.channels << "}\n";
  return 0;
}
