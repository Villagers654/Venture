#pragma once
// Diagnostic-only instruction-boundary observations. Private selectors and
// architectural state never belong in a public report. Default off.
#include "core/api_result_trace.h"
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <charconv>
#include <string_view>
#include <cerrno>

namespace venture::pc_trace {
struct Selection {
  std::array<std::uint64_t, 16> pcs{};
  std::size_t count{};
  bool contains(std::uint64_t pc) const noexcept {
    for (std::size_t i=0;i<count;++i) if(pcs[i]==pc)return true;
    return false;
  }
};
inline Selection parse(std::string_view text) noexcept {
  Selection out;
  if(text.empty())return out;
  while(!text.empty()) {
    const auto comma=text.find(','); auto token=text.substr(0,comma);
    if(token.starts_with("0x"))token.remove_prefix(2);
    std::uint64_t pc{};auto [end,ec]=std::from_chars(token.data(),token.data()+token.size(),pc,16);
    if(token.empty() || ec!=std::errc{} || end!=token.data()+token.size() || !pc || (pc&3))return {};
    if(!out.contains(pc)) { if(out.count==out.pcs.size())return {};out.pcs[out.count++]=pc; }
    if(comma==text.npos)break;
    text.remove_prefix(comma+1);if(text.empty())return {};
  }
  return out;
}
struct Snapshot { std::uint64_t sequence{},pc{},sp{},time_ns{},host_thread{};std::uint32_t nzcv{};std::array<std::uint64_t,31> x{}; };
template<std::size_t Capacity> class Ring {
  static_assert(Capacity>0);
  struct Slot { std::atomic<std::uint64_t> version{},sequence{},pc{},sp{},time_ns{},host_thread{};std::atomic<std::uint32_t> nzcv{};std::array<std::atomic<std::uint64_t>,31> x{}; };
  std::array<Slot,Capacity> slots_{};std::atomic<std::uint64_t> total_{};
public:
  void record(std::uint64_t pc,std::uint64_t sp,std::uint32_t nzcv,const std::array<std::uint64_t,31>& x,std::uint64_t host_thread=0,std::uint64_t time_ns=0) noexcept {
    const auto n=total_.load()+1;auto& s=slots_[(n-1)%Capacity];s.version.fetch_add(1);
    s.sequence.store(n);s.pc.store(pc);s.sp.store(sp);s.nzcv.store(nzcv);s.time_ns.store(time_ns);s.host_thread.store(host_thread);
    for(std::size_t i=0;i<x.size();++i)s.x[i].store(x[i]);
    s.version.fetch_add(1);total_.store(n);
  }
  std::uint64_t total()const noexcept{return total_.load();}
  template<class F> std::uint64_t visit(F&& f)const noexcept {
    std::uint64_t unstable=0;
    for(const auto& s:slots_) {
      auto v=s.version.load();if(v&1){++unstable;continue;}Snapshot r;
      r.sequence=s.sequence.load();r.pc=s.pc.load();r.sp=s.sp.load();r.nzcv=s.nzcv.load();r.time_ns=s.time_ns.load();r.host_thread=s.host_thread.load();
      for(std::size_t i=0;i<r.x.size();++i)r.x[i]=s.x[i].load();
      if(v!=s.version.load()){++unstable;continue;}if(r.sequence)f(r);
    }
    return unstable;
  }
};
inline constexpr std::size_t capacity=64,thread_capacity=64;
inline std::array<Ring<capacity>,thread_capacity> rings;
inline std::atomic<std::uint64_t> claimed{},dropped{};
inline std::atomic<bool> flushed{},frozen{};
inline const Selection& selection() {
  static const Selection value=[] {const auto* s=std::getenv("VENTURE_DIAGNOSTIC_PCS");return parse(s?s:"");}();
  return value;
}
inline std::size_t claim()noexcept{return claimed.fetch_add(1);}
inline void record(std::size_t slot,std::uint64_t pc,std::uint64_t sp,std::uint32_t nzcv,const std::array<std::uint64_t,31>& x,std::uint64_t host_thread,std::uint64_t time_ns)noexcept {
  if(frozen.load())return;
  if(slot<rings.size())rings[slot].record(pc,sp,nzcv,x,host_thread,time_ns);else dropped.fetch_add(1);
}
// Freeze before a chained guest handler can change the observed execution.
// Do not latch the normal logger into terminal mode here. The public signal
// summary and reporter must still run before crash-safe output is emitted.
inline void freeze()noexcept {
  for(std::size_t i=0;i<rings.size() && i<claimed.load();++i)
    if(rings[i].total()){frozen.store(true);return;}
}
inline void flush()noexcept {
  freeze();
  if(!claimed.load() || flushed.exchange(true))return;
  const int saved_errno=errno;
  using api_result_trace::Line;
  Line h;h.text("[pc-observer] snapshot");h.field(" slots=",claimed.load());h.field(" capacity=",capacity);h.field(" dropped=",dropped.load());h.write();
  for(std::size_t slot=0;slot<rings.size() && slot<claimed.load();++slot) {
    auto unstable=rings[slot].visit([&](const Snapshot& r)noexcept {
      Line l;l.text("[pc-observer]");l.field(" slot=",slot);l.field(" seq=",r.sequence);l.field(" pc=",r.pc);l.field(" sp=",r.sp);l.field(" nzcv=",r.nzcv);l.field(" host_thread=",r.host_thread);l.field(" time_ns=",r.time_ns);l.write();
      for(std::size_t i=0;i<r.x.size();i+=4){Line g;g.text("[pc-observer-registers]");g.field(" slot=",slot);g.field(" seq=",r.sequence);g.field(" first=",i);for(std::size_t j=i;j<r.x.size() && j<i+4;++j)g.field(" value=",r.x[j]);g.write();}
    });
    Line f;f.text("[pc-observer-summary]");f.field(" slot=",slot);f.field(" total=",rings[slot].total());f.field(" overwritten=",rings[slot].total()>capacity?rings[slot].total()-capacity:0);f.field(" unstable=",unstable);f.write();
  }
  errno=saved_errno;
}
}
