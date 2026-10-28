// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#include "dynarmic/interface/exclusive_monitor.h"

#include <algorithm>

#include "common/assert.h"

namespace Dynarmic {

ExclusiveMonitor::ExclusiveMonitor(std::size_t processor_count)
        : processors(processor_count) {}

size_t ExclusiveMonitor::GetProcessorCount() const {
    return processors.size();
}

void ExclusiveMonitor::Lock() {
    lock.Lock();
}

void ExclusiveMonitor::Unlock() {
    lock.Unlock();
}

bool ExclusiveMonitor::CheckAndClear(std::size_t processor_id, VAddr address) {
    const VAddr masked_address = address & RESERVATION_GRANULE_MASK;
    Lock();
    if (processors[processor_id].address != masked_address) {
        Unlock();
        return false;
    }
    for (size_t i = 0, count = ActiveCount(); i < count; ++i)
        if (processors[i].address == masked_address)
            processors[i].address = INVALID_EXCLUSIVE_ADDRESS;
    return true;
}

void ExclusiveMonitor::EnableGenerations() {
    generations.resize(GENERATION_BUCKETS);
}

void ExclusiveMonitor::Clear() {
    Lock();
    for (size_t i = 0, count = ActiveCount(); i < count; ++i) {
        processors[i].address = INVALID_EXCLUSIVE_ADDRESS;
    }
    Unlock();
}

void ExclusiveMonitor::ClearProcessor(size_t processor_id) {
    Lock();
    processors[processor_id].address = INVALID_EXCLUSIVE_ADDRESS;
    Unlock();
}

}  // namespace Dynarmic
