/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <cstdint>

namespace Dynarmic::FP {

class FPCR;
class FPSR;

template<typename FPT>
FPT FPRecipEstimate(FPT op, FPCR fpcr, FPSR& fpsr);

/// Venture: fraction bits of FPRecipEstimate<u32> for 1.0 + (i << 15), i = 0-255.
/// For a biased exponent E in 1-252 the estimate is sign | (253 - E) << 23 | table[fraction >> 15].
const std::uint32_t* FPRecipEstimateTable32();

}  // namespace Dynarmic::FP
