// The production Windows external passthrough source client (the temporary
// SBN1 compatibility transport) against a controlled server process.
#include "xr/passthrough_source.h"
#include "common/log.h"
#include <windows.h>
#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
namespace fs=std::filesystem;
using venture::xr::PassthroughSource;
using venture::xr::passthrough_source_answers;
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
unsigned serial=0;
struct Server {
  Handle ready,connected,resume;Process process;
  Server(const fs::path& image,const char* mode) {
    // Named events are only test synchronization.
    const auto key="Local\\VenturePassthroughSource."+std::to_string(GetCurrentProcessId())+"."+std::to_string(GetTickCount64())+"."+std::to_string(++serial);
    const auto first=key+".ready",second=key+".connected",third=key+".resume";
    ready.value=CreateEventA(nullptr,TRUE,FALSE,first.c_str());
    connected.value=CreateEventA(nullptr,TRUE,FALSE,second.c_str());
    resume.value=CreateEventA(nullptr,TRUE,FALSE,third.c_str());
    require(ready.value&&connected.value&&resume.value,"Cannot create fixture events");
    const std::string suffix=std::string(" ")+mode+" "+first+" "+second+" "+third;
    auto command=L"\""+image.wstring()+L"\""+std::wstring(suffix.begin(),suffix.end());
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION child{};
    require(CreateProcessW(image.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&child),"Cannot start the source fixture");
    process.value=child.hProcess;CloseHandle(child.hThread);
    if(WaitForSingleObject(ready.value,5000)!=WAIT_OBJECT_0) {
      DWORD code=0;GetExitCodeProcess(process.value,&code);
      throw std::runtime_error(std::string("Fixture pipe did not become ready (")+mode+", exit "+std::to_string(code)+")");
    }
  }
  void attach(){require(WaitForSingleObject(connected.value,5000)==WAIT_OBJECT_0,"The client did not connect to the source");}
  void kill(){if(process.value){TerminateProcess(process.value,0);WaitForSingleObject(process.value,5000);}}
  ~Server(){kill();}
};
void withdraw(PassthroughSource& source){
  source.request(false);require(source.permission()==0,"Opt-out did not revoke synchronously");
  await([&]{return source.withdrawn();},"Worker did not retire its cancelled connection",1200);
}
int main(int argc,char** argv) {
 try {
  require(argc==2,"Pass the controlled source executable");const auto image=fs::absolute(argv[1]);
  require(!passthrough_source_answers(300),"A source was reported without one listening");
  {
    Server server(image,"sbn1");
    require(passthrough_source_answers(800),"An answering SBN1 server failed the probe");
    server.kill();
    require(!passthrough_source_answers(300),"A source was reported after it exited");
  }
  for(const auto* mode:{"sbn1-v1","sbn1-denied"}) {
    // A refusal still answers: the server grants only once this process submits frames.
    Server server(image,mode);
    require(passthrough_source_answers(800),"An answering SBN1 server failed the probe");
  }
  for(const auto* mode:{"sbn1-silent","sbn1-malformed","sbn1-wrong-nonce","sbn1-wrong-pid","sbn1-wrong-server","sbn1-misnamed"}) {
    Server server(image,mode);const auto start=GetTickCount64();
    require(!passthrough_source_answers(400),"A silent or invalid SBN1 server passed the probe");
    require(GetTickCount64()-start<1500,"The SBN1 probe outlived its wait");
  }
  std::cout<<"PASS SBN1 probe\n";
  for(const auto* mode:{"sbn1","sbn1-v1","sbn1-partial"}) {
    Server server(image,mode);PassthroughSource source;
    require(source.withdrawn(),"A new client must be quiescent");source.request(true);
    await([&]{return source.permission()!=0;},"A whole or fragmented SBN1 grant was not admitted");
    const auto lease=source.permission();Sleep(2200);
    require(source.permission()==lease,"Healthy idle SBN1 lease expired");withdraw(source);
    std::cout<<"PASS "<<mode<<" admission, idle lease and withdrawal\n";
  }
  {
    Server server(image,"sbn1-toggle");PassthroughSource source;source.request(true);
    await([&]{return source.permission()!=0;},"No first SBN1 lease");const auto old=source.permission();
    await([&]{return source.permission()==0;},"A revoked SBN1 grant kept its lease");
    await([&]{return source.permission()!=0&&source.permission()!=old;},"A new SBN1 grant did not grant a fresh lease");
    withdraw(source);
    std::cout<<"PASS SBN1 grant changes on one connection\n";
  }
  for(const auto* mode:{"sbn1-denied","sbn1-malformed","sbn1-wrong-nonce","sbn1-wrong-pid","sbn1-wrong-server"}) {
    Server server(image,mode);PassthroughSource source;source.request(true);server.attach();Sleep(150);
    require(source.permission()==0,"A refused, malformed or mismatched SBN1 answer was admitted");withdraw(source);
    std::cout<<"PASS "<<mode<<" refusal\n";
  }
  {
    Server server(image,"sbn1-misnamed");PassthroughSource source;source.request(true);Sleep(600);
    require(source.permission()==0,"A pipe named for another process was admitted");withdraw(source);
    std::cout<<"PASS sbn1-misnamed refusal\n";
  }
  {
    Server server(image,"sbn1");PassthroughSource source;source.request(true);
    await([&]{return source.permission()!=0;},"No SBN1 grant before death test");server.kill();
    await([&]{return source.permission()==0;},"SBN1 server exit did not revoke permission");withdraw(source);
    std::cout<<"PASS SBN1 server exit revocation\n";
  }
  {
    Server server(image,"sbn1-reconnect");PassthroughSource source;source.request(true);
    await([&]{return source.permission()!=0;},"No first SBN1 connection");const auto old=source.permission();
    await([&]{return source.permission()==0;},"SBN1 disconnect did not revoke lease");
    SetEvent(server.resume.value);
    await([&]{return source.permission()!=0&&source.permission()!=old;},"SBN1 reconnect reused stale permission");withdraw(source);
    std::cout<<"PASS SBN1 reconnect with a fresh lease\n";
  }
  for(const auto* mode:{"sbn1-silent","sbn1-partial-hang","sbn1-delayed"}) {
    Server server(image,mode);PassthroughSource source;source.request(true);server.attach();
    withdraw(source);Sleep(250);require(source.permission()==0,"A late answer resurrected withdrawn intent");
    if(std::string(mode)=="sbn1-delayed") {
      source.request(true);await([&]{return source.permission()!=0;},"Fresh intent failed after cancellation");withdraw(source);
    }
    std::cout<<"PASS "<<mode<<" pending I/O cancellation and stale-answer rejection\n";
  }
  {
    Server server(image,"sbn1-partial-hang");const auto start=GetTickCount64();
    {PassthroughSource source;source.request(true);server.attach();}
    require(GetTickCount64()-start<2000,"Destructor did not cancel and drain pending I/O");
    std::cout<<"PASS destruction with pending I/O\n";
  }
  return 0;
 }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
