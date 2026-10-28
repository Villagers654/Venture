// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2018 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include <dynarmic/common/spin_lock.h>

namespace Dynarmic {

using VAddr = std::uint64_t;
using Vector = std::array<std::uint64_t, 2>;

class ExclusiveMonitor {
public:
    /// @param processor_count Maximum number of processors using this global
    ///                        exclusive monitor. Each processor must have a
    ///                        unique id.
    explicit ExclusiveMonitor(size_t processor_count);

    size_t GetProcessorCount() const;

    /// Marks a region containing [address, address+size) to be exclusive to
    /// processor processor_id.
    template<typename T, typename Function>
    T ReadAndMark(size_t processor_id, VAddr address, Function op) {
        static_assert(std::is_trivially_copyable_v<T>);
        const VAddr masked_address = address & RESERVATION_GRANULE_MASK;

        Lock();
        MarkActive(processor_id);
        processors[processor_id].address = masked_address;
        const T value = op();
        std::memcpy(processors[processor_id].value.data(), &value, sizeof(T));
        Unlock();
        return value;
    }

    /// Checks to see if processor processor_id has exclusive access to the
    /// specified region. If it does, executes the operation then clears
    /// the exclusive state for processors if their exclusive region(s)
    /// contain [address, address+size).
    template<typename T, typename Function>
        requires std::is_trivially_copyable_v<T>
    bool DoExclusiveOperation(size_t processor_id, VAddr address, Function op) {
        if (!CheckAndClear(processor_id, address)) {
            return false;
        }

        T saved_value;
        std::memcpy(&saved_value, processors[processor_id].value.data(), sizeof(T));
        const bool result = op(saved_value);

        Unlock();
        return result;
    }

    /// Serialize a software atomic with translated exclusive operations.
    template<typename Function>
    auto DoAtomicOperation(Function op) {
        Lock();
        for (size_t i = 0, count = ActiveCount(); i < count; ++i) {
            processors[i].address = INVALID_EXCLUSIVE_ADDRESS;
        }
        try {
            const auto result = op();
            Unlock();
            return result;
        } catch (...) {
            Unlock();
            throw;
        }
    }

    /// Unmark everything.
    void Clear();
    /// Unmark processor id
    void ClearProcessor(size_t processor_id);

    /// Venture: per-granule reservation generations for value-compared
    /// (Unsafe_IgnoreGlobalMonitor) fastmem exclusives. An exclusive load
    /// records its granule's generation; an exclusive store first advances it
    /// with a locked compare-exchange and fails if it moved, so a store after
    /// another CPU's successful exclusive store fails even when the value was
    /// restored (ABA). No lock and no processor scan. Ordinary stores and LSE
    /// atomics do not advance it. Call before compiling any code.
    void EnableGenerations();
    static constexpr size_t GENERATION_BUCKETS = 4096;

private:
    bool CheckAndClear(size_t processor_id, VAddr address);
    size_t ActiveCount() const { return active_count.load(std::memory_order_relaxed); }
    void MarkActive(size_t processor_id) {
        size_t count = active_count.load(std::memory_order_relaxed);
        while (count <= processor_id && !active_count.compare_exchange_weak(
                   count, processor_id + 1, std::memory_order_relaxed)) {}
    }

    void Lock();
    void Unlock();

    friend volatile int* GetExclusiveMonitorLockPointer(ExclusiveMonitor*);
    friend size_t GetExclusiveMonitorProcessorCount(ExclusiveMonitor*);
    friend VAddr* GetExclusiveMonitorAddressPointer(ExclusiveMonitor*, size_t index);
    friend Vector* GetExclusiveMonitorValuePointer(ExclusiveMonitor*, size_t index);
    friend const std::atomic<size_t>* GetExclusiveMonitorActiveCountPointer(ExclusiveMonitor*);
    friend VAddr* GetExclusiveMonitorAddressBase(ExclusiveMonitor*);
    friend size_t GetExclusiveMonitorProcessorStride();
    friend std::uint64_t* GetExclusiveMonitorGenerationBase(ExclusiveMonitor*);
    friend std::uint64_t* GetExclusiveMonitorGenerationSlot(ExclusiveMonitor*, size_t index);

    static constexpr VAddr RESERVATION_GRANULE_MASK = 0xFFFF'FFFF'FFFF'FFFFull;
    static constexpr VAddr INVALID_EXCLUSIVE_ADDRESS = 0xDEAD'DEAD'DEAD'DEADull;
    SpinLock lock;
    // Venture has one processor slot per guest thread, rather than four
    // emulated console cores. Separate each thread's reservation cache line.
    struct alignas(64) ProcessorState {
        VAddr address = INVALID_EXCLUSIVE_ADDRESS;
        std::uint64_t generation = 0; // recorded by the exclusive load
        alignas(16) Vector value{};
    };
    static_assert(sizeof(ProcessorState) == 64);
    std::vector<ProcessorState> processors;
    // Published before a slot can hold a reservation; slots above this bound
    // remain invalid. JIT scans load the bound while holding the monitor lock.
    std::atomic<size_t> active_count{0};
    struct alignas(64) Generation { std::uint64_t value = 0; };
    std::vector<Generation> generations;
};

}  // namespace Dynarmic
