// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <map>
#include <new>
#include <vector>

#include "common/assert.h"
#include <bit>
#include "common/common_types.h"

#include "dynarmic/backend/exception_handler.h"
#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/common/safe_ops.h"

using UBYTE = u8;

enum UNWIND_OPCODE {
    UWOP_PUSH_NONVOL = 0,
    UWOP_ALLOC_LARGE = 1,
    UWOP_ALLOC_SMALL = 2,
    UWOP_SET_FPREG = 3,
    UWOP_SAVE_NONVOL = 4,
    UWOP_SAVE_NONVOL_FAR = 5,
    UWOP_SAVE_XMM128 = 8,
    UWOP_SAVE_XMM128_FAR = 9,
    UWOP_PUSH_MACHFRAME = 10,
};

union UNWIND_CODE {
    struct {
        UBYTE CodeOffset;
        UBYTE UnwindOp : 4;
        UBYTE OpInfo : 4;
    } code;
    USHORT FrameOffset;
};

// UNWIND_INFO is a tail-padded structure
struct UNWIND_INFO {
    UBYTE Version : 3;
    UBYTE Flags : 5;
    UBYTE SizeOfProlog;
    UBYTE CountOfCodes;
    UBYTE FrameRegister : 4;
    UBYTE FrameOffset : 4;
    // UNWIND_CODE UnwindCode[];
    // With Flags == 0 there are no additional fields.
    // OPTIONAL UNW_EXCEPTION_INFO ExceptionInfo;
};

struct UNW_EXCEPTION_INFO {
    ULONG ExceptionHandler;
    // OPTIONAL ARBITRARY HandlerData;
};

namespace Dynarmic::Backend {

using namespace Dynarmic::Backend::X64;

struct PrologueInformation {
    std::vector<UNWIND_CODE> unwind_code;
    size_t number_of_unwind_code_entries;
    u8 prolog_size;
};

static PrologueInformation GetPrologueInformation(const ABIPrologue& prologue, bool body = false, bool helper = false) {
    PrologueInformation ret;

    const auto next_entry = [&]() -> UNWIND_CODE& {
        ret.unwind_code.emplace_back();
        return ret.unwind_code.back();
    };
    const auto push_nonvol = [&](u8 offset, u8 reg) {
        auto& entry = next_entry();
        entry.code.CodeOffset = offset;
        entry.code.UnwindOp = UWOP_PUSH_NONVOL;
        entry.code.OpInfo = reg;
    };
    const auto alloc_large = [&](u8 offset, size_t size) {
        ASSERT(size % 8 == 0);
        size /= 8;

        auto& entry = next_entry();
        entry.code.CodeOffset = offset;
        entry.code.UnwindOp = UWOP_ALLOC_LARGE;
        if (size <= 0xFFFF) {
            entry.code.OpInfo = 0;
            auto& size_entry = next_entry();
            size_entry.FrameOffset = static_cast<USHORT>(size);
        } else {
            entry.code.OpInfo = 1;
            auto& size_entry_1 = next_entry();
            size_entry_1.FrameOffset = static_cast<USHORT>(size);
            auto& size_entry_2 = next_entry();
            size_entry_2.FrameOffset = static_cast<USHORT>(size >> 16);
        }
    };
    const auto save_xmm128 = [&](u8 offset, u8 reg, size_t frame_offset) {
        ASSERT(frame_offset % 16 == 0);

        auto& entry = next_entry();
        entry.code.CodeOffset = offset;
        entry.code.UnwindOp = UWOP_SAVE_XMM128;
        entry.code.OpInfo = reg;
        auto& offset_entry = next_entry();
        offset_entry.FrameOffset = static_cast<USHORT>(frame_offset / 16);
    };

    // Reverse the actual emitter operations, including its AVX/SSE instruction
    // lengths and CalculateFrameInfo slots. Shared-body ranges have already
    // executed the whole Run/Step prologue, so all offsets there are zero.
    ret.prolog_size = body || prologue.operations.empty() ? 0 : prologue.operations.back().code_offset;
    for (auto it = prologue.operations.rbegin(); it != prologue.operations.rend(); ++it) {
        const u8 offset = body ? 0 : it->code_offset;
        switch (it->kind) {
        case ABIPrologue::Kind::Push:
            if (helper && (it->reg < 3 || (it->reg >= 8 && it->reg <= 11)))
                alloc_large(offset, 8); // Volatile pushes only change RSP.
            else push_nonvol(offset, it->reg);
            break;
        case ABIPrologue::Kind::Allocate: alloc_large(offset, it->stack_offset); break;
        case ABIPrologue::Kind::SaveXmm:
            if (!helper || it->reg >= 6) save_xmm128(offset, it->reg, it->stack_offset);
            break;
        }
    }

    ret.number_of_unwind_code_entries = ret.unwind_code.size();

    // The Windows API requires the size of the unwind_code array
    // to be a multiple of two for alignment reasons.
    if (ret.unwind_code.size() % 2 == 1) {
        auto& last_entry = next_entry();
        last_entry.FrameOffset = 0;
    }
    ASSERT(ret.unwind_code.size() % 2 == 0);

    return ret;
}

struct ExceptionHandler::Impl final {
    Impl(BlockOfCode& code) : code(code) {
        const auto prolog_info = GetPrologueInformation(code.GetRunCodePrologue());

        code.align(16);
        const u8* exception_handler_without_cb = code.getCurr<u8*>();
        code.mov(code.eax, static_cast<u32>(ExceptionContinueSearch));
        code.ret();
        code.RegisterNativeUnwindFrame(exception_handler_without_cb, code.getCurr(), {});

        code.align(16);
        const u8* exception_handler_with_cb = code.getCurr<u8*>();
        // Our 3rd argument is a PCONTEXT.

        // C++ exceptions and unwinding must continue searching. Only resumable
        // access faults at generated guest accesses belong to fastmem.
        code.cmp(code.dword[code.ABI_PARAM1 + Xbyak::RegExp(offsetof(EXCEPTION_RECORD, ExceptionCode))], EXCEPTION_ACCESS_VIOLATION);
        Xbyak::Label access_fault;
        code.je(access_fault);
        code.cmp(code.dword[code.ABI_PARAM1 + Xbyak::RegExp(offsetof(EXCEPTION_RECORD, ExceptionCode))], EXCEPTION_IN_PAGE_ERROR);
        code.jne(exception_handler_without_cb);
        code.L(access_fault);
        code.test(code.dword[code.ABI_PARAM1 + Xbyak::RegExp(offsetof(EXCEPTION_RECORD, ExceptionFlags))], EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND | EXCEPTION_NONCONTINUABLE);
        code.jnz(exception_handler_without_cb);

        // If not within our codeblock, ignore this exception.
        code.mov(code.rax, Safe::Negate(std::bit_cast<u64>(code.getCode())));
        code.add(code.rax, code.qword[code.ABI_PARAM3 + Xbyak::RegExp(offsetof(CONTEXT, Rip))]);
        code.cmp(code.rax, static_cast<u32>(code.GetTotalCodeSize()));
        code.ja(exception_handler_without_cb);

        ABIPrologue handler_prologue;
        // The OS caller's shadow space is above its return address. Reserve our
        // own shadow space before calling C++ from this independent native frame.
        code.sub(code.rsp, 8 + ABI_SHADOW_SPACE);
        handler_prologue.operations.push_back({ABIPrologue::Kind::Allocate,
                static_cast<u8>(code.getCurr() - exception_handler_with_cb), 0, 8 + ABI_SHADOW_SPACE});
        code.mov(code.ABI_PARAM1, std::bit_cast<u64>(&cb));
        code.mov(code.ABI_PARAM2, code.ABI_PARAM3);
        code.CallLambda(
            [](const std::function<FakeCall(u64)>& cb_, PCONTEXT ctx) {
                FakeCall fc = cb_(ctx->Rip);

                ctx->Rsp -= sizeof(u64);
                *std::bit_cast<u64*>(ctx->Rsp) = fc.ret_rip;
                ctx->Rip = fc.call_rip;
            }, false);  // SEH runs in host context; the fault trampoline flushes guest state.
        code.mov(code.eax, static_cast<u32>(ExceptionContinueExecution));
        code.add(code.rsp, 8 + ABI_SHADOW_SPACE);
        code.ret();
        code.RegisterNativeUnwindFrame(exception_handler_with_cb, code.getCurr(), handler_prologue);

        exception_handler_without_cb_offset = static_cast<ULONG>(exception_handler_without_cb - code.getCode<u8*>());
        exception_handler_with_cb_offset = static_cast<ULONG>(exception_handler_with_cb - code.getCode<u8*>());

        const DWORD entry_info = MakeUnwindInfo(prolog_info, true);
        const DWORD body_info = MakeUnwindInfo(GetPrologueInformation(code.GetRunCodePrologue(), true), true);
        const auto ranges = code.GetRunCodeRanges();
        // Chained body regions retain the primary Run frame's complete scope.
        code.align(4);
        auto* chain = static_cast<UNWIND_INFO*>(code.AllocateFromCodeSpace(sizeof(UNWIND_INFO)));
        auto* chain_data = code.GetWritablePointer(chain);
        *chain_data = {};
        chain_data->Version = 1;
        chain_data->Flags = UNW_FLAG_CHAININFO;
        auto* primary = static_cast<RUNTIME_FUNCTION*>(code.AllocateFromCodeSpace(sizeof(RUNTIME_FUNCTION)));
        *code.GetWritablePointer(primary) = {static_cast<DWORD>(ranges[2]), static_cast<DWORD>(code.GetTotalCodeSize()), body_info};
        body_gap_info = static_cast<DWORD>(reinterpret_cast<const u8*>(chain) - code.getCode());

        const auto ntdll = GetModuleHandleW(L"ntdll.dll");
        add_table = reinterpret_cast<AddTable>(GetProcAddress(ntdll, "RtlAddGrowableFunctionTable"));
        grow_table = reinterpret_cast<GrowTable>(GetProcAddress(ntdll, "RtlGrowFunctionTable"));
        table.delete_table = reinterpret_cast<DeleteTable>(GetProcAddress(ntdll, "RtlDeleteGrowableFunctionTable"));
        ASSERT(add_table && grow_table && table.delete_table);
        SYSTEM_INFO sys{};
        GetSystemInfo(&sys);
        page_size = sys.dwPageSize;
        // A native helper or inline outgoing frame consumes at least 8 bytes
        // of code and adds at most two entries. Reservation is commit-on-demand.
        capacity = static_cast<DWORD>(code.GetTotalCodeSize() / 4 + 3);
        rfuncs.reset(static_cast<RUNTIME_FUNCTION*>(VirtualAlloc(nullptr,
                size_t(capacity) * sizeof(RUNTIME_FUNCTION), MEM_RESERVE, PAGE_READWRITE)));
        if (!rfuncs) throw std::bad_alloc{};
        EnsureTableCommitted(3);
        rfuncs.get()[0] = {static_cast<DWORD>(ranges[0]), static_cast<DWORD>(ranges[1]), entry_info};
        rfuncs.get()[1] = {static_cast<DWORD>(ranges[1]), static_cast<DWORD>(ranges[2]), entry_info};
        rfuncs.get()[2] = {static_cast<DWORD>(ranges[2]), static_cast<DWORD>(code.GetTotalCodeSize()), body_gap_info};
        count = 3;
        for (const auto& frame : code.GetPendingNativeUnwindFrames())
            AddFrame(frame.begin, frame.end, frame.prologue, frame.inherits_run_frame);
        permanent_count = count;
        permanent_tail = rfuncs.get()[count - 1];
        // Automatic clears can run inside the generated dispatcher's lookup
        // callback. Keep its complete prelude registered even if re-registration
        // of the recyclable table fails while that native frame is active.
        permanent_rfuncs.assign(rfuncs.get(), rfuncs.get() + permanent_count);
        permanent_rfuncs.back().EndAddress = static_cast<DWORD>(code.getCurr() - code.getCode());
        RegisterTable();
        if (!RtlAddFunctionTable(permanent_rfuncs.data(), permanent_count,
                reinterpret_cast<DWORD64>(code.getCode()))) throw std::bad_alloc{};
        permanent_table.entries = permanent_rfuncs.data();
        code.SetNativeUnwindCallbacks(
                [this](const void* begin, const void* end, const ABIPrologue& prologue, bool inherits_run_frame) { AddFrame(begin, end, prologue, inherits_run_frame); },
                [this](size_t begin) { ResetFrames(begin); });
    }

    void SetCallback(std::function<FakeCall(u64)> new_cb) {
        cb = new_cb;
        for (auto* info : except_infos)
            info->ExceptionHandler = cb ? exception_handler_with_cb_offset : exception_handler_without_cb_offset;
    }

    ~Impl() {
        code.SetNativeUnwindCallbacks({}, {});
    }

private:
    using AddTable = LONG (NTAPI*)(PVOID*, PRUNTIME_FUNCTION, DWORD, DWORD, ULONG_PTR, ULONG_PTR);
    using GrowTable = void (NTAPI*)(PVOID, DWORD);
    using DeleteTable = void (NTAPI*)(PVOID);
    struct TableOwner {
        PVOID handle = nullptr;
        DeleteTable delete_table = nullptr;
        ~TableOwner() { if (handle) delete_table(handle); }
    };
    struct PermanentTableOwner {
        RUNTIME_FUNCTION* entries = nullptr;
        ~PermanentTableOwner() { if (entries) RtlDeleteFunctionTable(entries); }
    };
    struct TableMemoryDeleter {
        void operator()(RUNTIME_FUNCTION* p) const { if (p) VirtualFree(p, 0, MEM_RELEASE); }
    };

    DWORD MakeUnwindInfo(const PrologueInformation& info, bool handler, bool inherits_run_frame = false) {
        code.align(4);
        auto* unwind_info = static_cast<UNWIND_INFO*>(code.AllocateFromCodeSpace(sizeof(UNWIND_INFO)));
        auto* data = code.GetWritablePointer(unwind_info);
        *data = {};
        data->Version = 1;
        data->Flags = inherits_run_frame ? UNW_FLAG_CHAININFO : handler ? UNW_FLAG_EHANDLER : 0;
        data->SizeOfProlog = inherits_run_frame ? 0 : info.prolog_size;
        data->CountOfCodes = inherits_run_frame ? 0 : static_cast<UBYTE>(info.number_of_unwind_code_entries);
        const size_t bytes = inherits_run_frame ? 0 : sizeof(UNWIND_CODE) * info.unwind_code.size();
        auto* unwind_code = code.AllocateFromCodeSpace(bytes);
        if (bytes) memcpy(code.GetWritablePointer(unwind_code), info.unwind_code.data(), bytes);
        if (inherits_run_frame) {
            auto* primary = static_cast<RUNTIME_FUNCTION*>(code.AllocateFromCodeSpace(sizeof(RUNTIME_FUNCTION)));
            // Windows restores XMMs before applying the combined allocation.
            // Describe the enclosing frame relative to the current, lowered RSP.
            ASSERT(info.number_of_unwind_code_entries == 2 &&
                   info.unwind_code[0].code.UnwindOp == UWOP_ALLOC_LARGE &&
                   info.unwind_code[0].code.OpInfo == 0);
            const size_t extra = size_t(info.unwind_code[1].FrameOffset) * 8;
            ABIPrologue combined = code.GetRunCodePrologue();
            for (auto& operation : combined.operations) {
                if (operation.kind == ABIPrologue::Kind::Allocate || operation.kind == ABIPrologue::Kind::SaveXmm)
                    operation.stack_offset += extra;
            }
            const DWORD combined_info = MakeUnwindInfo(GetPrologueInformation(combined, true), true);
            const auto ranges = code.GetRunCodeRanges();
            *code.GetWritablePointer(primary) = {static_cast<DWORD>(ranges[2]), static_cast<DWORD>(code.GetTotalCodeSize()), combined_info};
        }
        if (handler) {
            auto* except_info = code.GetWritablePointer(static_cast<UNW_EXCEPTION_INFO*>(code.AllocateFromCodeSpace(sizeof(UNW_EXCEPTION_INFO))));
            except_info->ExceptionHandler = cb ? exception_handler_with_cb_offset : exception_handler_without_cb_offset;
            except_infos.push_back(except_info);
        }
        return static_cast<DWORD>(reinterpret_cast<const u8*>(unwind_info) - code.getCode());
    }

    void EnsureTableCommitted(DWORD entries) {
        ASSERT(entries <= capacity);
        const size_t required = (size_t(entries) * sizeof(RUNTIME_FUNCTION) + page_size - 1) / page_size * page_size;
        if (required > committed) {
            if (!VirtualAlloc(rfuncs.get(), required, MEM_COMMIT, PAGE_READWRITE)) throw std::bad_alloc{};
            committed = required;
        }
    }

    void RegisterTable() {
        const auto base = reinterpret_cast<ULONG_PTR>(code.getCode());
        if (add_table(&table.handle, rfuncs.get(), count, capacity, base, base + code.GetTotalCodeSize()) < 0)
            throw std::bad_alloc{};
    }

    void AddFrame(const void* begin, const void* end, const ABIPrologue& prologue, bool inherits_run_frame) {
        const DWORD first = static_cast<DWORD>(static_cast<const u8*>(begin) - code.getCode());
        const DWORD last = static_cast<DWORD>(static_cast<const u8*>(end) - code.getCode());
        ASSERT(first >= rfuncs.get()[count - 1].BeginAddress && last > first);
        const auto info = GetPrologueInformation(prologue, false, true);
        std::vector<USHORT> key{static_cast<USHORT>(info.prolog_size), static_cast<USHORT>(info.number_of_unwind_code_entries), static_cast<USHORT>(inherits_run_frame)};
        for (const auto& entry : info.unwind_code) key.push_back(entry.FrameOffset);
        auto found = helper_infos.find(key);
        if (found == helper_infos.end())
            found = helper_infos.emplace(std::move(key), MakeUnwindInfo(info, false, inherits_run_frame)).first;
        EnsureTableCommitted(count + 2);
        if (first > rfuncs.get()[count - 1].BeginAddress) {
            rfuncs.get()[count - 1].EndAddress = first;
            rfuncs.get()[count++] = {first, last, found->second};
        } else {
            rfuncs.get()[count - 1] = {first, last, found->second};
        }
        rfuncs.get()[count++] = {last, static_cast<DWORD>(code.GetTotalCodeSize()), body_gap_info};
        if (table.handle) grow_table(table.handle, count);
    }

    void ResetFrames(size_t begin) {
        // Retain the permanent prelude table while recycling emitted code.
        if (table.handle) {
            table.delete_table(table.handle);
            table.handle = nullptr;
        }
        count = permanent_count;
        rfuncs.get()[count - 1] = permanent_tail;
        for (auto it = helper_infos.begin(); it != helper_infos.end(); ) {
            if (it->second >= begin) it = helper_infos.erase(it);
            else ++it;
        }
        std::erase_if(except_infos, [&](const auto* info) {
            return reinterpret_cast<const u8*>(info) - code.GetWritablePointer(code.getCode()) >= static_cast<ptrdiff_t>(begin);
        });
        // If registration fails, ClearCache propagates and its pending invalidation
        // remains set: the next Run retries before it can execute any JIT code.
        RegisterTable();
    }

    BlockOfCode& code;
    std::unique_ptr<RUNTIME_FUNCTION, TableMemoryDeleter> rfuncs;
    TableOwner table; // Delete the OS table before freeing its backing memory.
    std::vector<RUNTIME_FUNCTION> permanent_rfuncs;
    PermanentTableOwner permanent_table; // Delete before its vector is destroyed.
    AddTable add_table = nullptr;
    GrowTable grow_table = nullptr;
    DWORD capacity = 0, count = 0, permanent_count = 0, body_gap_info = 0;
    size_t page_size = 0, committed = 0;
    RUNTIME_FUNCTION permanent_tail{};
    std::map<std::vector<USHORT>, DWORD> helper_infos;
    std::function<FakeCall(u64)> cb;
    std::vector<UNW_EXCEPTION_INFO*> except_infos;
    ULONG exception_handler_without_cb_offset;
    ULONG exception_handler_with_cb_offset;

};

ExceptionHandler::ExceptionHandler() = default;
ExceptionHandler::~ExceptionHandler() = default;

void ExceptionHandler::Register(BlockOfCode& code) {
    impl = std::make_unique<Impl>(code);
}

bool ExceptionHandler::SupportsFastmem() const noexcept {
    return static_cast<bool>(impl);
}

void ExceptionHandler::SetFastmemCallback(std::function<FakeCall(u64)> cb) {
    impl->SetCallback(cb);
}

}  // namespace Dynarmic::Backend
