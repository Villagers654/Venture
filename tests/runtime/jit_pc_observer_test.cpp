#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <dynarmic/backend/x64/a64_jitstate.h>
#include <array>
#include <optional>
#include <string>
#include <cstring>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>
using namespace Dynarmic;
using u64=std::uint64_t;using u32=std::uint32_t;
unsigned checks=0;
void check(bool x){++checks;if(!x)throw std::runtime_error("synthetic PC observer assertion "+std::to_string(checks));}
struct Sample{u64 pc,sp;u32 nzcv;std::array<u64,31>x;};
struct Env final:A64::UserCallbacks {
 std::unique_ptr<A64::Jit> jit;std::vector<Sample> samples;std::vector<u64> selected;
 std::optional<u32> MemoryReadCode(u64 p)override{u32 w;std::memcpy(&w,reinterpret_cast<void*>(p),4);return w;}
 u64 MemoryRead(u64,size_t)override{throw std::runtime_error("unexpected memory read");}
 void MemoryWrite(u64,u64,size_t)override{throw std::runtime_error("unexpected memory write");}
 A64::Vector MemoryRead128(u64)override{throw std::runtime_error("unexpected vector read");}
 void MemoryWrite128(u64,A64::Vector)override{throw std::runtime_error("unexpected vector write");}
 bool MemoryWriteExclusive64(u64,u64,u64)override{throw std::runtime_error("unexpected exclusive");}
 u64 MemoryAtomic(u64,size_t,A64::AtomicOp,u64,u64)override{throw std::runtime_error("unexpected atomic");}
 A64::Vector MemoryAtomicCompareAndSwap128(u64,A64::Vector,A64::Vector)override{throw std::runtime_error("unexpected atomic128");}
 void CallSVC(u32)override{jit->HaltExecution(HaltReason::UserDefined1);}
 void ExceptionRaised(u64,A64::Exception)override{throw std::runtime_error("unexpected guest exception");}
 void AddTicks(u64)override{}u64 GetTicksRemaining()override{return 100000;}u64 GetCNTPCT()override{return 0;}
};
int main(){try{
 // Original instructions are identical in all modes. A selected PC lies in
 // the middle of what would otherwise be one translated block.
 const std::array<u32,8> code{0x91000529,0x9100094a,0xeb0a013f,0x54000062,0xd2800020,0x14000002,0xd2800040,0xd4000001};
 const auto base=reinterpret_cast<u64>(code.data());
 for(bool cached:{false,true})for(bool enabled:{false,true})for(unsigned scenario=0;scenario<3;++scenario){
  ExclusiveMonitor monitor(1);Env e;e.samples.reserve(32);e.selected={base+8,base+12,base+16,base+24,base+28};
  A64::UserConfig c;c.fastmem_pointer=0;c.fastmem_address_space_bits=64;c.silently_mirror_fastmem=false;c.callbacks=&e;c.global_monitor=&monitor;c.processor_id=0;c.enable_cycle_counting=false;c.enable_register_cache=cached;c.enable_bounded_halt_polling=true;c.nzcv_exit_liveness=true;c.fast_dispatch_table_bits=12;
  if(enabled){c.diagnostic_entry.selected=[&](u64 pc){for(auto p:e.selected)if(p==pc)return true;return false;};c.diagnostic_entry.user=&e;
   c.diagnostic_entry.record=[](void* user,u64 pc,const void* raw)noexcept{
    auto& e=*static_cast<Env*>(user);const auto& s=*static_cast<const Backend::X64::A64JitState*>(raw);
    e.samples.push_back({pc,s.sp,s.GetPstate(),s.reg});
#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
    asm volatile("movabs $0x1234567812345678, %%r10\n\tmov %%r10, %%r11":::"r10","r11");
#endif
   };
  }
  e.jit=std::make_unique<A64::Jit>(c);
  for(unsigned repeat=0;repeat<2;++repeat){
   e.samples.clear();e.jit->ClearHalt(HaltReason::UserDefined1);
   e.jit->SetRegister(9,scenario==1?9:4);e.jit->SetRegister(10,scenario==2?3:5);e.jit->SetRegister(0,99);e.jit->SetRegister(20,0xaabb);e.jit->SetRegister(30,0xccdd);e.jit->SetSP(0x100000);e.jit->SetPstate(0);e.jit->SetFpcr(0x400000);e.jit->SetPC(base);
   auto halt=e.jit->Run();check(Has(halt,HaltReason::UserDefined1));check(e.jit->GetRegister(0)==(scenario==0?1:2));check(e.jit->GetRegister(9)==(scenario==1?10:5));check(e.jit->GetRegister(10)==(scenario==2?5:7));check(e.jit->GetRegister(20)==0xaabb);check(e.jit->GetRegister(30)==0xccdd);check(e.jit->GetSP()==0x100000);check(e.jit->GetFpcr()==0x400000);
   check(e.samples.size()==(enabled?4:0));
   if(enabled){check(e.samples[0].pc==base+8);check(e.samples[0].x[9]==(scenario==1?10:5));check(e.samples[0].x[10]==(scenario==2?5:7));check(e.samples[1].pc==base+12);check(e.samples[1].nzcv==(scenario==0?0x80000000u:scenario==1?0x20000000u:0x60000000u));check(e.samples[2].pc==base+(scenario==0?16:24));check(e.samples[3].x[0]==(scenario==0?1:2));for(const auto& r:e.samples){check(r.sp==0x100000);check(r.x[20]==0xaabb);check(r.x[30]==0xccdd);}}
  }
 }
 std::printf("PASS: %u genuine JIT assertions; observer off/on, register cache off/on, less/greater/equal, cold/warm executions\n",checks);return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
