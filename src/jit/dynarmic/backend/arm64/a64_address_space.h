// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <utility>
#include <vector>

#include "dynarmic/backend/arm64/address_space.h"
#include "dynarmic/backend/block_range_information.h"
#include "dynarmic/interface/A64/config.h"

namespace Dynarmic::Backend::Arm64 {

struct EmittedBlockInfo;

class A64AddressSpace final : public AddressSpace {
public:
    explicit A64AddressSpace(const A64::UserConfig& conf);

    void GenerateIR(IR::Block& ir_block, IR::LocationDescriptor) const override;

    void InvalidateCacheRanges(const boost::icl::interval_set<u64>& ranges);

protected:
    friend class A64Core;

    void EmitPrelude();
    bool RegisterCacheEnabled() const;
    void EmitRegisterCacheLoad();
    void EmitRegisterCacheStore();
    EmitConfig GetEmitConfig() override;
    void RegisterNewBasicBlock(const IR::Block& block, const EmittedBlockInfo& block_info) override;
    u64 DescriptorPC(IR::LocationDescriptor descriptor) const override;
    bool IsA32() const override { return false; }
    std::string FriendlyName(IR::LocationDescriptor descriptor) const override;
    void ClearBlockRanges() override { block_ranges.ClearCache(); }

    const A64::UserConfig conf;
    BlockRangeInformation<u64> block_ranges;
    // Venture: the outlined atomic helpers the block being compiled translated in place.
    mutable std::vector<std::pair<u64, u64>> inlined_code;
};

}  // namespace Dynarmic::Backend::Arm64
