// A controlled external server. Its private copy is registered as vrserver.exe
// in an isolated LOCALAPPDATA; production discovery and PID/path checks stay on.
#include "xr/steam_link_protocol.h"
#include <windows.h>
#include <string>
#include <array>
namespace wire = venture::xr::steam_link;
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
  if(argc!=4)return 2;
  const std::string mode=argv[1];
  HANDLE ready=OpenEventA(EVENT_MODIFY_STATE,FALSE,argv[2]);
  HANDLE connected=OpenEventA(EVENT_MODIFY_STATE,FALSE,argv[3]);
  if(!ready||!connected)return 3;
  const auto name=L"\\\\.\\pipe\\SFNP.StreamBridge.V1."+std::to_wstring(GetCurrentProcessId());
  unsigned connections=0;
  for(;;) {
    HANDLE pipe=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT,1,4096,4096,0,nullptr);
    if(pipe==INVALID_HANDLE_VALUE)return 4;
    SetEvent(ready);
    const bool attached=ConnectNamedPipe(pipe,nullptr)||GetLastError()==ERROR_PIPE_CONNECTED;
    wire::Record record{};
    if(attached&&read_all(pipe,record.data(),DWORD(record.size()))) {
      SetEvent(connected);++connections;
      wire::write(record,4,2,2);const bool budget=wire::read(record,6,2)==2;
      wire::write(record,10,2,budget?5:1);wire::write(record,20,4,GetCurrentProcessId());
      record[40]=5;wire::write(record,56,8,17);
      if(mode=="malformed")record[0]^=128;
      if(mode=="wrong-nonce")record[24]^=128;
      if(mode=="wrong-pid")wire::write(record,20,4,GetCurrentProcessId()+1);
      if(mode=="delayed")Sleep(200);
      if(mode!="no-reply") {
        if(mode=="partial"||mode=="partial-hang") {
          write_all(pipe,record.data(),17);
          if(mode=="partial") {Sleep(30);write_all(pipe,record.data()+17,DWORD(record.size()-17));}
        } else write_all(pipe,record.data(),DWORD(record.size()));
      }
      if(mode=="reconnect"&&connections==1)Sleep(400);
      else { unsigned char stop{};DWORD count{};ReadFile(pipe,&stop,1,&count,nullptr); }
    }
    DisconnectNamedPipe(pipe);CloseHandle(pipe);
  }
}
