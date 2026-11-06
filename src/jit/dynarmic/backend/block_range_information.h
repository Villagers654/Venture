// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <vector>

#include <boost/icl/interval_set.hpp>
#include "common/container/unordered_map.h"
#include "common/container/unordered_set.h"

#include "dynarmic/ir/location_descriptor.h"

namespace Dynarmic::Backend {

/// Guest code ranges of compiled blocks, bucketed by 4 KiB page. A block's range is
/// recorded in every page it touches; invalidation drops the entries it returns.
template<typename P>
class BlockRangeInformation {
public:
    void AddRange(boost::icl::discrete_interval<P> range, IR::LocationDescriptor location);
    void ClearCache();
    ::Common::unordered_set<IR::LocationDescriptor> InvalidateRanges(const boost::icl::interval_set<P>& ranges);

private:
    struct Entry {
        P first, last;
        IR::LocationDescriptor location;
    };
    static constexpr unsigned page_bits = 12;
    ::Common::unordered_map<P, std::vector<Entry>> pages;
};

}  // namespace Dynarmic::Backend
