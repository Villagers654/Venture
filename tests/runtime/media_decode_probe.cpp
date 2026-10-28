// Offline verification of the same bounded MOV/AAC path used by the players.
// Inputs and decoded outputs stay in the caller's owned evidence directory.
#include "media/media.h"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <memory>
#include <atomic>
#include <chrono>
#include <thread>

int main(int argc, char** argv) {
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
