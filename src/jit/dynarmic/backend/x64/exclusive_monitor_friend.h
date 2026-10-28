/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include "dynarmic/interface/exclusive_monitor.h"

namespace Dynarmic {

inline volatile int* GetExclusiveMonitorLockPointer(ExclusiveMonitor* monitor) {
    return &monitor->lock.storage;
}

inline size_t GetExclusiveMonitorProcessorCount(ExclusiveMonitor* monitor) {
    return monitor->processors.size();
}

inline VAddr* GetExclusiveMonitorAddressPointer(ExclusiveMonitor* monitor, size_t index) {
    monitor->MarkActive(index);
    return &monitor->processors[index].address;
}

inline const std::atomic<size_t>* GetExclusiveMonitorActiveCountPointer(ExclusiveMonitor* monitor) {
    static_assert(std::atomic<size_t>::is_always_lock_free);
    static_assert(sizeof(std::atomic<size_t>) == sizeof(size_t));
    return &monitor->active_count;
}

inline VAddr* GetExclusiveMonitorAddressBase(ExclusiveMonitor* monitor) {
    return &monitor->processors[0].address;
}

inline size_t GetExclusiveMonitorProcessorStride() {
    return sizeof(ExclusiveMonitor::ProcessorState);
}

inline std::uint64_t* GetExclusiveMonitorGenerationBase(ExclusiveMonitor* monitor) {
    static_assert(sizeof(ExclusiveMonitor::Generation) == 64);
    return monitor->generations.empty() ? nullptr : &monitor->generations[0].value;
}

inline std::uint64_t* GetExclusiveMonitorGenerationSlot(ExclusiveMonitor* monitor, size_t index) {
    return &monitor->processors[index].generation;
}

inline Vector* GetExclusiveMonitorValuePointer(ExclusiveMonitor* monitor, size_t index) {
    return &monitor->processors[index].value;
}

}  // namespace Dynarmic
