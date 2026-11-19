// SPDX-License-Identifier: GPL-3.0-or-later
#include "android_game/platform/a32_call.h"

#include <cstring>
#include <deque>
#include <vector>

#include "hle/ovrplatform/abi.h"

namespace venture::ovrplatform::a32_native {
namespace {
// The pinned SDK's ILP32 records (OVR_Types.h, OVR_Matchmaking*.h).
struct KeyValuePair32 {
  std::uint32_t key;
  std::int32_t type;
  std::uint32_t string_value;
  std::int32_t int_value;
  double double_value;
};
struct MatchmakingCriterion32 {
  std::uint32_t key;
  std::int32_t importance;
  std::uint32_t parameters, count;
};
struct MatchmakingQuery32 {
  std::uint32_t data, data_count, criteria, criteria_count;
};
static_assert(sizeof(KeyValuePair32) == 24 && offsetof(KeyValuePair32, double_value) == 16);
static_assert(sizeof(MatchmakingCriterion32) == 16 && sizeof(MatchmakingQuery32) == 16);
// No SDK call takes more; a larger count is a bad argument.
constexpr std::uint32_t kMaxElements = 4096;

struct Fault {};

// AAPCS32 base standard, stage C: doublewords take an even register pair or
// an 8-byte aligned stack slot, and once anything is on the stack every later
// argument is too.
class Words {
 public:
  Words(const std::uint32_t* frame, std::uint64_t stack, const Memory& memory)
      : frame_(frame), stack_(stack), memory_(memory) {}
  void skip_register() { ++ncrn_; }
  std::uint32_t word() {
    if (ncrn_ < 4) return frame_[ncrn_++];
    std::uint32_t value;
    read(stack_, &value, 4);
    stack_ += 4;
    return value;
  }
  std::uint64_t pair() {
    ncrn_ += ncrn_ & 1;
    if (ncrn_ <= 2) {
      const std::uint64_t value = frame_[ncrn_] | std::uint64_t(frame_[ncrn_ + 1]) << 32;
      ncrn_ += 2;
      return value;
    }
    ncrn_ = 4;
    stack_ = (stack_ + 7) & ~std::uint64_t(7);
    std::uint64_t value;
    read(stack_, &value, 8);
    stack_ += 8;
    return value;
  }
  void read(std::uint64_t address, void* out, std::size_t size) const {
    if (!memory_.read(address, out, size)) throw Fault{};
  }

 private:
  const std::uint32_t* frame_;
  std::uint64_t stack_;
  const Memory& memory_;
  unsigned ncrn_ = 0;
};

// The A64 call the handler sees (AAPCS64): integers in x0-x7, floating point
// in v0-v7, the rest in 8-byte stack slots.
class A64 {
 public:
  explicit A64(NativeContext& context) : context_(context) {}
  void integer(std::uint64_t value) {
    if (nx_ < 8) context_.x[nx_++] = value;
    else stack_.push_back(value);
  }
  void floating(std::uint64_t bits) {
    if (nv_ < 8) {
      context_.q[nv_][0] = bits;
      context_.q[nv_++][1] = 0;
    } else
      stack_.push_back(bits);
  }
  void finish() {
    if (!stack_.empty()) context_.sp = reinterpret_cast<std::uintptr_t>(stack_.data());
  }

 private:
  NativeContext& context_;
  unsigned nx_ = 0, nv_ = 0;
  std::vector<std::uint64_t> stack_;
};

std::uint64_t address(const void* host) { return reinterpret_cast<std::uintptr_t>(host); }

// The A64 copies of P, K and Q arguments, alive for the call.
class Copies {
 public:
  explicit Copies(const Words& words) : words_(words) {}
  std::uint64_t pointers(std::uint32_t array, std::uint32_t count) {
    if (!array) return 0;
    if (count > kMaxElements) throw Fault{};
    std::vector<std::uint32_t> narrow(count);
    if (count) words_.read(array, narrow.data(), 4ull * count);
    auto& wide = pointer_arrays_.emplace_back(narrow.begin(), narrow.end());
    return address(wide.data());
  }
  std::uint64_t pairs(std::uint32_t array, std::uint32_t count) {
    if (!array) return 0;
    if (count > kMaxElements) throw Fault{};
    std::vector<KeyValuePair32> narrow(count);
    if (count) words_.read(array, narrow.data(), sizeof(KeyValuePair32) * count);
    auto& wide = pair_arrays_.emplace_back(count);
    for (std::uint32_t i = 0; i < count; ++i)
      wide[i] = {narrow[i].key, narrow[i].type, 0, narrow[i].string_value, narrow[i].int_value, 0,
                 narrow[i].double_value};
    return address(wide.data());
  }
  std::uint64_t query(std::uint32_t pointer) {
    if (!pointer) return 0;
    MatchmakingQuery32 narrow;
    words_.read(pointer, &narrow, sizeof(narrow));
    if (narrow.criteria_count > kMaxElements) throw Fault{};
    std::vector<MatchmakingCriterion32> criteria(narrow.criteria_count);
    if (!criteria.empty() && narrow.criteria)
      words_.read(narrow.criteria, criteria.data(), sizeof(MatchmakingCriterion32) * criteria.size());
    auto& wide = criteria_arrays_.emplace_back(criteria.size());
    for (std::size_t i = 0; i < criteria.size(); ++i)
      wide[i] = {criteria[i].key, criteria[i].importance, 0, pairs(criteria[i].parameters, criteria[i].count),
                 criteria[i].count, 0};
    auto& out = queries_.emplace_back();
    out = {pairs(narrow.data, narrow.data_count), narrow.data_count, 0,
           narrow.criteria ? address(wide.data()) : 0, narrow.criteria_count, 0};
    return address(&out);
  }

 private:
  const Words& words_;
  std::deque<std::vector<std::uint64_t>> pointer_arrays_;
  std::deque<std::vector<KeyValuePair64>> pair_arrays_;
  std::deque<std::vector<MatchmakingCriterion64>> criteria_arrays_;
  std::deque<MatchmakingQuery64> queries_;
};
}  // namespace

bool call(std::string_view signature, std::uint32_t id, std::uint32_t* frame, std::uint64_t stack,
          const Memory& memory, const std::function<void(NativeContext&)>& run) {
  if (signature.empty()) {
    frame[0] = frame[1] = 0;
    return false;
  }
  const char result = signature.front();
  const auto arguments = signature.substr(1);
  try {
    Words words(frame, stack, memory);
    if (result == 'C') {
      // ovrKeyValuePair_makeString(key, value): r0 is the result's address.
      const std::uint32_t out = frame[0];
      words.skip_register();
      const KeyValuePair32 pair{words.word(), 0, words.word(), 0, 0.0};
      if (!out || !memory.write(out, &pair, sizeof(pair))) throw Fault{};
      return true;
    }
    // Each argument's words first: an array's count is the argument after it.
    std::vector<std::uint64_t> raw(arguments.size());
    for (std::size_t i = 0; i < arguments.size(); ++i)
      raw[i] = arguments[i] == 'I' || arguments[i] == 'd' ? words.pair() : words.word();

    alignas(16) NativeContext context{};
    context.x[16] = id;
    A64 a64(context);
    Copies copies(words);
    for (std::size_t i = 0; i < arguments.size(); ++i) {
      const auto count = [&] { return i + 1 < raw.size() ? std::uint32_t(raw[i + 1]) : 0u; };
      switch (arguments[i]) {
        case 'i': a64.integer(std::uint64_t(std::int64_t(std::int32_t(raw[i])))); break;
        case 'u': case 'I': a64.integer(raw[i]); break;
        case 'f': a64.floating(std::uint32_t(raw[i])); break;
        case 'd': a64.floating(raw[i]); break;
        case 'P': a64.integer(copies.pointers(std::uint32_t(raw[i]), count())); break;
        case 'K': a64.integer(copies.pairs(std::uint32_t(raw[i]), count())); break;
        case 'Q': a64.integer(copies.query(std::uint32_t(raw[i]))); break;
        default: throw Fault{};
      }
    }
    a64.finish();
    run(context);
    switch (result) {
      case 'v': frame[0] = frame[1] = 0; break;
      case 'w': frame[0] = std::uint32_t(context.x[0]); break;
      case 'B': frame[0] = std::uint32_t(context.x[0] & 0xff); break;
      case 'I': frame[0] = std::uint32_t(context.x[0]), frame[1] = std::uint32_t(context.x[0] >> 32); break;
      case 'f': frame[0] = std::uint32_t(context.q[0][0]); break;
      case 'd': frame[0] = std::uint32_t(context.q[0][0]), frame[1] = std::uint32_t(context.q[0][0] >> 32); break;
      default: throw Fault{};
    }
    return true;
  } catch (const Fault&) {
    frame[0] = frame[1] = 0;
    return false;
  }
}
}  // namespace venture::ovrplatform::a32_native
