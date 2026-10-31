#include "xr/steam_link.h"
#include "common/log.h"
#include <windows.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <functional>
#include <stdexcept>
#include <string>
namespace fs=std::filesystem;
using venture::xr::SteamLinkSource;
namespace venture::log {
Level level(){return Level::warn;}
void write(Level,std::string_view,std::string_view){}
}
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void await(const std::function<bool()>& ready,const char* message,DWORD timeout=5000){
  const auto end=GetTickCount64()+timeout;
  while(!ready()){require(GetTickCount64()<end,message);Sleep(5);}
}
struct Handle {
  HANDLE value=nullptr;
  explicit Handle(HANDLE handle=nullptr):value(handle){}
  ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
  Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
struct Process : Handle {
  ~Process(){if(value){TerminateProcess(value,0);WaitForSingleObject(value,5000);}}
};
struct Registry {
  fs::path root,image;std::wstring original;
  Registry(const fs::path& server) {
    wchar_t prior[32768]{};const auto length=GetEnvironmentVariableW(L"LOCALAPPDATA",prior,32768);
    require(length<32768,"LOCALAPPDATA is too long");if(length)original.assign(prior,length);
    root=fs::temp_directory_path()/("venture-sbn1-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    require(fs::create_directory(root),"Cannot create private fixture root");
    image=root/"runtime/bin/win64/vrserver.exe";fs::create_directories(image.parent_path());fs::copy_file(server,image);
    fs::create_directories(root/"openvr");const auto text=(root/"runtime").u8string();
    const std::string runtime(reinterpret_cast<const char*>(text.data()),text.size());
    std::ofstream(root/"openvr/openvrpaths.vrpath")<<nlohmann::json{{"jsonid","vrpathreg"},{"version",1},{"runtime",{runtime}}};
    require(SetEnvironmentVariableW(L"LOCALAPPDATA",root.c_str()),"Cannot isolate fixture registry");
  }
  ~Registry(){SetEnvironmentVariableW(L"LOCALAPPDATA",original.empty()?nullptr:original.c_str());std::error_code ignored;fs::remove_all(root,ignored);}
};
struct Server {
  Handle ready,connected;Process process;
  Server(const Registry& registry,const char* mode):
    ready(CreateEventW(nullptr,TRUE,FALSE,nullptr)),connected(CreateEventW(nullptr,TRUE,FALSE,nullptr)) {
    // Named events are only test synchronization; the production pipe remains PID-bound.
    const auto key="Local\\VentureSbn1."+std::to_string(GetCurrentProcessId())+"."+std::to_string(GetTickCount64());
    const auto first=key+".ready",second=key+".connected";
    CloseHandle(ready.value);ready.value=CreateEventA(nullptr,TRUE,FALSE,first.c_str());
    CloseHandle(connected.value);connected.value=CreateEventA(nullptr,TRUE,FALSE,second.c_str());
    require(ready.value&&connected.value,"Cannot create fixture events");
    const std::string suffix=std::string(" ")+mode+" "+first+" "+second;
    auto command=L"\""+registry.image.wstring()+L"\""+std::wstring(suffix.begin(),suffix.end());
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};
    require(CreateProcessW(registry.image.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child),"Cannot start owned server fixture");
    process.value=child.hProcess;CloseHandle(child.hThread);
    require(WaitForSingleObject(ready.value,5000)==WAIT_OBJECT_0,"Fixture pipe did not become ready");
  }
  void attach(){require(WaitForSingleObject(connected.value,5000)==WAIT_OBJECT_0,"Production client did not connect to verified fixture");}
  void kill(){if(process.value){TerminateProcess(process.value,0);WaitForSingleObject(process.value,5000);}}
  ~Server(){kill();}
};
void withdraw(SteamLinkSource& source){
  source.request(false);require(source.permission()==0,"Opt-out did not revoke synchronously");
  await([&]{return source.withdrawn();},"Worker did not retire its cancelled pipe",1200);
}
int main(int argc,char** argv) {
 try {
  require(argc==2,"Pass the controlled server executable");Registry registry(fs::absolute(argv[1]));
  for(const auto* mode:{"healthy","partial"}) {
    Server server(registry,mode);SteamLinkSource source;
    require(source.withdrawn(),"A new client must be quiescent");source.request(true);
    await([&]{return source.permission()!=0;},"Valid or fragmented reply was not admitted");
    const auto lease=source.permission();Sleep(2200);
    require(source.permission()==lease,"Healthy idle lease expired");withdraw(source);
    std::cout<<"PASS "<<mode<<" admission, idle lease and withdrawal\n";
  }
  for(const auto* mode:{"no-reply","partial-hang","delayed"}) {
    Server server(registry,mode);SteamLinkSource source;source.request(true);server.attach();
    withdraw(source);Sleep(250);require(source.permission()==0,"Late reply resurrected withdrawn intent");
    if(std::string(mode)=="delayed") {
      source.request(true);await([&]{return source.permission()!=0;},"Fresh intent failed after cancellation");withdraw(source);
    }
    std::cout<<"PASS "<<mode<<" pending I/O cancellation and stale-reply rejection\n";
  }
  for(const auto* mode:{"malformed","wrong-nonce","wrong-pid"}) {
    Server server(registry,mode);SteamLinkSource source;source.request(true);server.attach();Sleep(150);
    require(source.permission()==0,"Malformed or mismatched reply was admitted");withdraw(source);
    std::cout<<"PASS "<<mode<<" refusal\n";
  }
  {
    Server server(registry,"healthy");SteamLinkSource source;source.request(true);
    await([&]{return source.permission()!=0;},"No grant before death test");server.kill();
    await([&]{return source.permission()==0;},"Server death did not revoke permission");withdraw(source);
    std::cout<<"PASS server death revocation\n";
  }
  {
    Server server(registry,"reconnect");SteamLinkSource source;source.request(true);
    await([&]{return source.permission()!=0;},"No first connection");const auto old=source.permission();
    await([&]{return source.permission()==0;},"Disconnect did not revoke lease");
    await([&]{return source.permission()!=0&&source.permission()!=old;},"Reconnect reused stale permission");withdraw(source);
    std::cout<<"PASS reconnect with a fresh lease\n";
  }
  {
    Server server(registry,"partial-hang");const auto start=GetTickCount64();
    {SteamLinkSource source;source.request(true);server.attach();}
    require(GetTickCount64()-start<2000,"Destructor did not cancel and drain pending I/O");
    std::cout<<"PASS destruction with pending I/O\n";
  }
  return 0;
 }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
