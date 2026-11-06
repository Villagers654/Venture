// A controlled external passthrough source: a separate process serving the
// temporary SBN1 compatibility pipe as this user, answering in one test mode.
#include <windows.h>
#include <string>
#include "sbn1_server.h"
bool read_all(HANDLE pipe, void* bytes, DWORD length) {
  auto* data=static_cast<unsigned char*>(bytes);
  for(DWORD used=0;used<length;) { DWORD got=0;if(!ReadFile(pipe,data+used,length-used,&got,nullptr)||!got)return false;used+=got; }
  return true;
}
bool write_all(HANDLE pipe, const void* bytes, DWORD length) {
  auto* data=static_cast<const unsigned char*>(bytes);
  for(DWORD used=0;used<length;) { DWORD got=0;if(!WriteFile(pipe,data+used,length-used,&got,nullptr)||!got)return false;used+=got; }
  return true;
}
int main(int argc,char** argv) {
  if(argc!=5)return 2;
  const std::string mode=argv[1];
  HANDLE ready=OpenEventA(EVENT_MODIFY_STATE,FALSE,argv[2]);
  HANDLE connected=OpenEventA(EVENT_MODIFY_STATE,FALSE,argv[3]);
  HANDLE resume=OpenEventA(SYNCHRONIZE,FALSE,argv[4]);
  if(!ready||!connected||!resume)return 3;
  return sbn1_fixture::run(mode,ready,connected,resume);
}
