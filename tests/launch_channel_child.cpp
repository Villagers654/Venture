#include "common/ipc.h"
#include "common/launch_details.h"
int main(int argc, char** argv) {
  try {
    std::uintptr_t r=0,w=0;
    for (int i=1;i+1<argc;++i) {
      const std::string option=argv[i];
      if(option=="--credential-read") r=std::stoull(argv[++i]);
      else if(option=="--credential-write") w=std::stoull(argv[++i]);
    }
    auto pipe=venture::PipeChannel::inherited(r,w);
    auto startup=pipe->call({{"op","launch_details"}});
    auto details=venture::read_launch_details(startup.at("details"));
    auto reply=pipe->call({{"op","launch_other_app"},{"app","123"},
                           {"details",venture::launch_details_json(details)}});
    return reply.value("success",false)?0:1;
  } catch (...) { return 2; }
}
