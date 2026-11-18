// JIT defaults that depend on the host CPU. Each choice is made once per
// process, is logged, and keeps its VENTURE_JIT_* override for comparisons.
#pragma once

namespace venture::host_tuning {
// How a guest return finds its next block (Dynarmic A64 inline_rsb_pop and
// compact_rsb_pop). `shared`: one shared lookup and indirect jump for every
// return. `per_site`: each return pops the RSB with its own jump, which gives
// the host branch predictor one entry per return site. `compact`: each return
// calls one shared lookup and jumps on its result (candidate).
enum class ReturnPop { shared, per_site, compact };
// VENTURE_JIT_INLINE_RSB_POP when set ("1" per site, "compact", anything
// else shared); otherwise the measured default for this host CPU.
ReturnPop return_pop() noexcept;
}  // namespace venture::host_tuning
