// Exact lane-extraction fixtures: poisoned upper lanes, shared source uses,
// integer and scalar-FP consumers. Expected bits are independent of the JIT.
#include <dynarmic/interface/A64/a64.h>
#include <dynarmic/interface/exclusive_monitor.h>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <utility>
namespace {
using namespace Dynarmic;
constexpr std::uint64_t base = 0x10000;
struct Callbacks : A64::UserCallbacks {
  std::vector<std::uint32_t> words;
  A64::Jit* jit = nullptr;
  std::optional<std::uint32_t> MemoryReadCode(std::uint64_t p) override {
    if (p < base || (p - base) / 4 >= words.size() || p % 4) return {};
    return words[(p - base) / 4];
  }
  std::uint64_t MemoryRead(std::uint64_t, std::size_t) override { throw std::runtime_error("unexpected read"); }
  A64::Vector MemoryRead128(std::uint64_t) override { throw std::runtime_error("unexpected read128"); }
  void MemoryWrite(std::uint64_t, std::uint64_t, std::size_t) override { throw std::runtime_error("unexpected write"); }
  void MemoryWrite128(std::uint64_t, A64::Vector) override { throw std::runtime_error("unexpected write128"); }
  void InterpreterFallback(std::uint64_t, std::size_t) { throw std::runtime_error("unexpected fallback"); }
  void ExceptionRaised(std::uint64_t, A64::Exception) override { throw std::runtime_error("unexpected exception"); }
  void CallSVC(std::uint32_t) override { jit->HaltExecution(HaltReason::UserDefined1); }
  void AddTicks(std::uint64_t) override {}
  std::uint64_t GetTicksRemaining() override { return 1000; }
  std::uint64_t MemoryAtomic(std::uint64_t, std::size_t, A64::AtomicOp, std::uint64_t, std::uint64_t) override { throw std::runtime_error("unexpected atomic"); }
  A64::Vector MemoryAtomicCompareAndSwap128(std::uint64_t, A64::Vector, A64::Vector) override { throw std::runtime_error("unexpected atomic128"); }
  std::uint64_t GetCNTPCT() override { return 0; }
};

void require(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
// Reuse the translator for each fixed optimizer/memory configuration. Every
// fixture still resets and checks its full architectural state.
struct Cpu {
  Callbacks cb;
  A64::Jit jit;
  static A64::UserConfig configuration(Callbacks& cb, bool alias, bool passes, bool cache, bool halt_check) {
    A64::UserConfig config;
    config.callbacks = &cb;
    config.enable_cycle_counting = false;
    config.check_halt_on_memory_access = halt_check;
    config.enable_register_cache = cache;
    if (cache) { config.fastmem_pointer = 0; config.fastmem_address_space_bits = 64; config.silently_mirror_fastmem = false; }
    config.code_cache_size = 16 * 1024 * 1024;
    config.optimizations = passes ? all_safe_optimizations : no_optimizations;
    if (alias) config.optimizations |= OptimizationFlag::LowLaneAliasing;
    else config.optimizations &= ~OptimizationFlag::LowLaneAliasing;
    return config;
  }
  Cpu(bool alias, bool passes, bool cache, bool halt_check)
      : jit(configuration(cb, alias, passes, cache, halt_check)) { cb.jit = &jit; }
  void program(std::vector<std::uint32_t> words) {
    if (cb.words != words) {
      cb.words = std::move(words);
      jit.ClearCache();
    }
    jit.ClearHalt(HaltReason::UserDefined1);
  }
};
void fixture(Cpu& cpu, bool fp, bool nan, std::uint32_t bits, std::uint32_t fpcr) {
  auto& cb = cpu.cb;
  auto& jit = cpu.jit;
  cpu.program(fp ? std::vector<std::uint32_t>{
      0x1e212802, // fadd s2,s0,s1
      0x1e240843, // fmul s3,s2,s4
      0x1e260040, 0x1e260061, 0x1e260002, // fmov w0,s2; w1,s3; w2,s0
      0xd4000021} : std::vector<std::uint32_t>{
      0x1e260000, // fmov w0,s0
      0x11000401, // add w1,w0,#1
      0x1e270022, // fmov s2,w1
      0x0e0c3c02, 0x0e143c03, 0x0e1c3c04, // umov w2..4,v0.s[1..3]
      0x1e260005, // fmov w5,s0 (original vector still live)
      0x4f07e7e3, // movi v3.16b,#0xff
      0x4e041c03, // ins v3.s[0],w0
      0xd4000021});
  std::array<std::uint64_t,31> expected_x;
  std::array<A64::Vector,32> expected_v;
  for (unsigned i=0;i<31;++i) { expected_x[i]=0xfedcba9800000000ull+i; jit.SetRegister(i,expected_x[i]); }
  for (unsigned i=0;i<32;++i) { expected_v[i]={0x0123456789abcdefull+i,0xfedcba9876543210ull-i}; }
  expected_v[0]={0x1122334400000000ull|bits,0x99aabbcc55667788ull};
  expected_v[1]={0xdeadbeef40100000ull,0xa5a5a5a55a5a5a5aull}; // 2.25f, dirty upper lanes
  expected_v[4]={0x0123456740000000ull,0xabcdef0123456789ull}; // 2.0f
  for(unsigned i=0;i<32;++i) jit.SetVector(i,expected_v[i]);
  auto fpsr=std::uint32_t{0x08000010};
  jit.SetFpcr(fpcr); jit.SetFpsr(fpsr); jit.SetPstate(0xa0000000); jit.SetSP(0x40000); jit.SetPC(base);
  if (fp) {
    // 1.5 + 2.25 = 3.75; 3.75 * 2 = 7.5, exact in every rounding mode.
    auto add=std::uint32_t{0x40700000},mul=std::uint32_t{0x40f00000};
    if(nan) {
      // A64 quiets this signaling NaN; DN selects the architectural default.
      add=mul=(fpcr & (1u<<25)) ? 0x7fc00000 : (bits | 0x00400000);
      fpsr |= 1;
    }
    expected_x[0]=add; expected_x[1]=mul; expected_x[2]=bits;
    expected_v[2]={add,0}; expected_v[3]={mul,0};
  } else {
    expected_x[0]=bits; expected_x[1]=std::uint32_t(bits+1);
    expected_x[2]=0x11223344; expected_x[3]=0x55667788; expected_x[4]=0x99aabbcc; expected_x[5]=bits;
    expected_v[2]={std::uint32_t(bits+1),0};
    expected_v[3]={0xffffffff00000000ull|bits,0xffffffffffffffffull};
  }
  const auto reason=jit.Run();
  require(Has(reason,HaltReason::UserDefined1),"SVC halt");
  for(unsigned i=0;i<31;++i) require(jit.GetRegister(i)==expected_x[i],"GPR bits");
  for(unsigned i=0;i<32;++i) require(jit.GetVector(i)==expected_v[i],"vector bits");
  require(jit.GetPC()==base+4*cb.words.size(),"exit PC");
  require(jit.GetSP()==0x40000 && jit.GetPstate()==0xa0000000,"SP/NZCV");
  require(jit.GetFpcr()==fpcr && jit.GetFpsr()==fpsr,"FPCR/FPSR");
}
// Scalar ops whose source's other lanes hold signaling NaNs: no lane but the
// low one may reach an x86 instruction that raises Invalid (FPSR.IOC).
void upper_snan_fixture(Cpu& cpu, std::uint32_t fpcr) {
  auto& jit = cpu.jit;
  cpu.program({0x1e21c002, 0x1e254003, // fsqrt s2,s0; frintm s3,s0
              0x1e61c0c5, 0x1e6540c7, // fsqrt d5,d6; frintm d7,d6
              0xd4000021});
  std::array<A64::Vector,32> expected_v;
  for (unsigned i=0;i<32;++i) expected_v[i]={0x0123456789abcdefull+i,0xfedcba9876543210ull-i};
  expected_v[0]={0x7fa1234540800000ull,0x7fa123457fa12345ull}; // 4.0f under three SNaN lanes
  expected_v[6]={0x4010000000000000ull,0x7ff4000000000001ull}; // 4.0 under an SNaN lane
  for(unsigned i=0;i<32;++i) jit.SetVector(i,expected_v[i]);
  const std::uint32_t fpsr=0x08000000;
  jit.SetFpcr(fpcr); jit.SetFpsr(fpsr); jit.SetPC(base);
  expected_v[2]={0x40000000,0}; expected_v[3]={0x40800000,0};
  expected_v[5]={0x4000000000000000ull,0}; expected_v[7]={0x4010000000000000ull,0};
  require(Has(jit.Run(),HaltReason::UserDefined1),"SVC halt");
  for(unsigned i=0;i<32;++i) require(jit.GetVector(i)==expected_v[i],"upper-lane SNaN vector bits");
  require(jit.GetFpsr()==fpsr,"upper-lane SNaN FPSR");
}
}
int main() {
 try {
  unsigned cases=0;
  for(bool passes:{false,true}) for(bool alias:{false,true}) for(bool cache:{false,true}) {
   Cpu scalar(alias,passes,cache,true), upper(alias,passes,cache,false);
   for(unsigned rm=0;rm<4;++rm) for(unsigned control=0;control<4;++control) {
    const auto fpcr=(rm<<22)|(control<<24);
    for(auto bits:{0u,1u,0x80000000u,0x7f800000u,0x7fa12345u,0xffcabcdeu}) {
     fixture(scalar,false,false,bits,fpcr);++cases;
    }
    fixture(scalar,true,false,0x3fc00000,fpcr);++cases;
    fixture(scalar,true,true,0x7fa12345,fpcr);++cases;
    upper_snan_fixture(upper,fpcr);++cases;
   }
  }
  std::printf("low-lane fixtures: %u passed; alias and optimizer on/off, both memory modes, independent full-state expectations\n",cases);
  return 0;
 } catch(const std::exception& e) {std::fprintf(stderr,"%s\n",e.what());return 1;}
}
