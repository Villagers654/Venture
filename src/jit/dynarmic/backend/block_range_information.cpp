// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/backend/block_range_information.h"

#include <boost/icl/interval_set.hpp>
#include "common/common_types.h"
#include "common/container/unordered_set.h"

namespace Dynarmic::Backend {

template<typename P>
void BlockRangeInformation<P>::AddRange(boost::icl::discrete_interval<P> range, IR::LocationDescriptor location) {
    const P first = boost::icl::first(range), last = boost::icl::last(range);
    if (last < first)
        return;
    for (P page = first >> page_bits;; ++page) {
        auto& entries = pages[page];
        // Blocks recompiled after an invalidation that missed this page add the same entry again.
        if (entries.empty() || entries.back().location != location || entries.back().first != first || entries.back().last != last)
            entries.push_back({first, last, location});
        if (page == last >> page_bits)
            break;
    }
}

template<typename P>
void BlockRangeInformation<P>::ClearCache() {
    pages.clear();
}

template<typename P>
::Common::unordered_set<IR::LocationDescriptor> BlockRangeInformation<P>::InvalidateRanges(const boost::icl::interval_set<P>& ranges) {
    ::Common::unordered_set<IR::LocationDescriptor> erase_locations;
    const auto sweep = [&](std::vector<Entry>& entries, P first, P last) {
        for (size_t i = 0; i < entries.size();) {
            if (entries[i].first <= last && entries[i].last >= first) {
                erase_locations.insert(entries[i].location);
                entries[i] = entries.back();
                entries.pop_back();
            } else {
                ++i;
            }
        }
    };
    for (auto invalidate_interval : ranges) {
        const P first = boost::icl::first(invalidate_interval), last = boost::icl::last(invalidate_interval);
        if (last < first)
            continue;
        const u64 page_count = u64(last >> page_bits) - u64(first >> page_bits) + 1;
        if (page_count > pages.size()) {
            // A range wider than the cache's pages (an unmap of a large region): visit the buckets instead.
            for (auto it = pages.begin(); it != pages.end();) {
                const P page = it->first;
                if (page >= (first >> page_bits) && page <= (last >> page_bits))
                    sweep(it->second, first, last);
                it = it->second.empty() ? pages.erase(it) : std::next(it);
            }
            continue;
        }
        for (P page = first >> page_bits;; ++page) {
            if (const auto it = pages.find(page); it != pages.end()) {
                sweep(it->second, first, last);
                if (it->second.empty())
                    pages.erase(it);
            }
            if (page == last >> page_bits)
                break;
        }
    }
    return erase_locations;
}

template class BlockRangeInformation<u32>;
template class BlockRangeInformation<u64>;

}  // namespace Dynarmic::Backend
