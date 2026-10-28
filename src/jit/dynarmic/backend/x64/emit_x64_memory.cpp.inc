// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2022 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#define CONCATENATE_TOKENS(x, y) CONCATENATE_TOKENS_IMPL(x, y)
#define CONCATENATE_TOKENS_IMPL(x, y) x##y

#define AxxEmitX64 CONCATENATE_TOKENS(Axx, EmitX64)
#define AxxEmitContext CONCATENATE_TOKENS(Axx, EmitContext)
#define AxxJitState CONCATENATE_TOKENS(Axx, JitState)
#define AxxUserConfig Axx::UserConfig

namespace {
using Vector = std::array<u64, 2>;
}

std::optional<AxxEmitX64::DoNotFastmemMarker> AxxEmitX64::ShouldFastmem(AxxEmitContext& ctx, IR::Inst* inst) const {
    if (!conf.fastmem_pointer || !exception_handler.SupportsFastmem()) {
        return std::nullopt;
    }

    const auto marker = std::make_tuple(ctx.Location(), inst->GetName());
    if (do_not_fastmem.count(marker) > 0) {
        return std::nullopt;
    }
    return marker;
}

void (*AxxEmitX64::FastmemFallback(FallbackKind kind, const FallbackKey& key))() {
    auto& fallbacks = kind == FallbackKind::Read ? read_fallbacks : kind == FallbackKind::Write ? write_fallbacks : exclusive_write_fallbacks;
    if (const auto it = fallbacks.find(key); it != fallbacks.end()) {
        return it->second;
    }
    // Generated at most once per key: the arena never outgrows the full set.
    const FallbackRequest request{kind, key};
    fallback_request = &request;
    try {
        code.EmitInThunkArena([this] { GenFastmemFallbacks(); });
    } catch (...) {
        fallback_request = nullptr;
        throw;
    }
    fallback_request = nullptr;
    const auto it = fallbacks.find(key);
    ASSERT_MSG(it != fallbacks.end(), "no fastmem fallback for this register pair");
    return it->second;
}

FakeCall AxxEmitX64::FastmemCallback(u64 rip_) {
    const auto iter = fastmem_patch_info.find(rip_);
    if (iter != fastmem_patch_info.end()) {
        FakeCall result{
            .call_rip = iter->second.callback,
            .ret_rip = iter->second.resume_rip,
        };
        if (iter->second.recompile) {
            const auto marker = iter->second.marker;
            do_not_fastmem.insert(marker);
            InvalidateBasicBlocks({std::get<0>(marker)});
        }
        return result;
    }
    fmt::print("dynarmic: Segfault happened within JITted code at rip = {:016x}\n"
        "Segfault wasn't at a fastmem patch location!\n", rip_);
    UNREACHABLE(); //("iter != fastmem_patch_info.end()");
}

namespace {
std::optional<s32> FoldedDisplacement(const EmitContext& ctx, const IR::Inst* inst) {
    const IR::Value address = inst->GetArg(1);
    if (address.IsImmediate() || !ctx.IsFoldedAddress(address.GetInst())) return std::nullopt;
    return static_cast<s32>(address.GetInst()->GetArg(1).GetImmediateAsU64());
}
}  // namespace

void AxxEmitX64::EmitFastmemFallback(AxxEmitContext& ctx, IR::Inst* inst, Xbyak::Label& abort, Xbyak::Label& end, Xbyak::Reg64 vaddr,
                                     std::optional<s32> folded, void (*wrapped_fn)(), const void* location, DoNotFastmemMarker marker) {
    // A folded access holds only the base in vaddr. The fallback reads the full
    // address from vaddr, so add the displacement around it and restore the base
    // before any abort exit. The fault entry tail-jumps, keeping the fake call's frame.
    code.L(abort);
    if (folded) code.lea(vaddr, code.ptr[vaddr + *folded]);
    code.call(wrapped_fn);
    const auto resume = std::bit_cast<u64>(code.getCurr());
    if (folded) code.lea(vaddr, code.ptr[vaddr - *folded]);
    EmitCheckMemoryAbort(ctx, inst, &end);
    code.jmp(end, code.T_NEAR);
    auto entry = std::bit_cast<u64>(wrapped_fn);
    if (folded) {
        entry = std::bit_cast<u64>(code.getCurr());
        code.lea(vaddr, code.ptr[vaddr + *folded]);
        code.jmp(reinterpret_cast<const void*>(wrapped_fn));
    }
    fastmem_patch_info.emplace(std::bit_cast<u64>(location), FastmemPatchInfo{resume, entry, marker, conf.recompile_on_fastmem_failure});
}

template<std::size_t bitsize, auto callback>
void AxxEmitX64::EmitMemoryRead(AxxEmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());
    const auto fastmem_marker = ShouldFastmem(ctx, inst);

    if (!conf.page_table && !fastmem_marker) {
        // Neither fastmem nor page table: Use callbacks
        if constexpr (bitsize == 128) {
            ctx.reg_alloc.HostCall(code, nullptr, {}, args[1]);
            if (ordered) code.mfence();
            code.CallFunction(memory_read_128);
            ctx.reg_alloc.DefineValue(code, inst, xmm1);
        } else {
            ctx.reg_alloc.HostCall(code, inst, {}, args[1]);
            code.mov(code.ABI_PARAM3.cvt32(), bitsize / CHAR_BIT);
            if (ordered) code.mfence();
            Devirtualize<callback>(conf.callbacks).EmitCall(code);
            code.ZeroExtendFrom(bitsize, code.ABI_RETURN);
        }
        EmitCheckMemoryAbort(ctx, inst);
        return;
    }

    if (ordered && bitsize == 128) {
        // Required for atomic 128-bit loads/stores
        code.FlushA64CachedRbx();
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RBX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RCX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RDX);
    }

    const auto folded = FoldedDisplacement(ctx, inst);
    const Xbyak::Reg64 vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
    // Venture: an FP load reads straight into an XMM register (MOVD/MOVQ zero the upper lanes).
    const bool to_xmm = (bitsize == 32 || bitsize == 64) && fastmem_marker && !ordered && xmm_read_fallbacks && LoadFeedsXmm(ctx, inst);
    const int value_idx = bitsize == 128 || to_xmm ? ctx.reg_alloc.ScratchXmm(code).getIdx() : ctx.reg_alloc.ScratchGpr(code).getIdx();

    const auto wrapped_fn = FastmemFallback(FallbackKind::Read, FallbackKey(ordered, to_xmm ? bitsize | xmm_read_fallback : bitsize, vaddr.getIdx(), value_idx));

    auto emit_read = [&](const Xbyak::RegExp& address) {
        if (to_xmm) {
            const void* location = code.getCurr();
            if constexpr (bitsize == 32) {
                code.movd(Xbyak::Xmm{value_idx}, dword[address]);
            } else if constexpr (bitsize == 64) {
                code.movq(Xbyak::Xmm{value_idx}, qword[address]);
            }
            return location;
        }
        if constexpr (bitsize > 8 && bitsize < 128) {
            if (ordered) {
                // Preserve the old atomic access for misaligned addresses.
                // MOV supplies atomicity only for naturally aligned operands.
                auto misaligned = ctx.GenSharedLabel(), resume = ctx.GenSharedLabel();
                code.test(vaddr.cvt32(), bitsize / 8 - 1);
                code.jnz(*misaligned, code.T_NEAR);
                const auto location = EmitReadMemoryMov<bitsize>(code, value_idx, address, ordered);
                code.L(*resume);
                ctx.deferred_emits.emplace_back([=, this] {
                    code.L(*misaligned);
                    const auto locked_location = EmitReadMemoryMov<bitsize>(code, value_idx, address, ordered, true);
                    if (fastmem_marker) {
                        fastmem_patch_info.emplace(std::bit_cast<u64>(locked_location), FastmemPatchInfo{
                            std::bit_cast<u64>(code.getCurr()), std::bit_cast<u64>(wrapped_fn),
                            *fastmem_marker, conf.recompile_on_fastmem_failure});
                    }
                    code.jmp(*resume);
                });
                return location;
            }
        }
        return EmitReadMemoryMov<bitsize>(code, value_idx, address, ordered);
    };

    SharedLabel abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();

    if (fastmem_marker) {
        // Use fastmem
        bool require_abort_handling = false;
        const auto src_ptr = EmitFastmemVAddr(code, ctx, *abort, vaddr, require_abort_handling);

        const auto location = emit_read(folded ? src_ptr + *folded : src_ptr);

        ctx.deferred_emits.emplace_back([=, this, &ctx] {
            EmitFastmemFallback(ctx, inst, *abort, *end, vaddr, folded, wrapped_fn, location, *fastmem_marker);
        });
    } else {
        // Use page table
        ASSERT(conf.page_table);
        const auto src_ptr = EmitVAddrLookup(code, ctx, bitsize, *abort, vaddr);
        emit_read(src_ptr);

        ctx.deferred_emits.emplace_back([=, this, &ctx] {
            code.L(*abort);
            code.call(wrapped_fn);
            EmitCheckMemoryAbort(ctx, inst, end);
            code.jmp(*end, code.T_NEAR);
        });
    }
    code.L(*end);
    if (ordered && bitsize == 128) code.ReloadA64CachedRbx();

    if (bitsize == 128 || to_xmm) {
        ctx.reg_alloc.DefineValue(code, inst, Xbyak::Xmm{value_idx});
    } else {
        ctx.reg_alloc.DefineValue(code, inst, Xbyak::Reg64{value_idx});
    }
}

template<std::size_t bitsize, auto callback>
void AxxEmitX64::EmitMemoryWrite(AxxEmitContext& ctx, IR::Inst* inst) {
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());
    const auto fastmem_marker = ShouldFastmem(ctx, inst);

    if (!conf.page_table && !fastmem_marker) {
        // Neither fastmem nor page table: Use callbacks
        if constexpr (bitsize == 128) {
            // Keep the address locked while spilling other live values for
            // the call. Releasing it first lets relocation reuse ABI_PARAM2
            // and overwrite the address of this store.
            ctx.reg_alloc.HostCall(code, nullptr, {}, args[1]);
            ctx.reg_alloc.Release(xmm1);
            // Copy, as HostCall does for its arguments: the call clobbers XMM1,
            // and the value may stay in use (an A64ColdSet* abort record).
            ctx.reg_alloc.UseScratch(code, args[2], HostLoc::XMM1);
            code.CallFunction(memory_write_128);
        } else {
            // { this, vaddr, value, size }
            ctx.reg_alloc.HostCall(code, nullptr, {}, args[1], args[2]);
            code.mov(code.ABI_PARAM4.cvt32(), bitsize / CHAR_BIT);
            Devirtualize<callback>(conf.callbacks).EmitCall(code);
        }
        if (ordered) code.mfence();
        EmitCheckMemoryAbort(ctx, inst);
        return;
    }

    if (ordered && bitsize == 128) {
        // Required for atomic 128-bit loads/stores
        code.FlushA64CachedRbx();
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RBX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RCX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RDX);
    }

    auto folded = FoldedDisplacement(ctx, inst);
    Xbyak::Reg64 vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
    const int value_idx = bitsize == 128
                            ? ctx.reg_alloc.UseXmm(code, args[2]).getIdx()
                            : (ordered ? ctx.reg_alloc.UseScratchGpr(code, args[2]).getIdx() : ctx.reg_alloc.UseGpr(code, args[2]).getIdx());

    if (folded && bitsize != 128 && value_idx == vaddr.getIdx()) {
        // The fault path adjusts vaddr in place; it must not also hold the stored value.
        const Xbyak::Reg64 address = ctx.reg_alloc.ScratchGpr(code);
        code.lea(address, code.ptr[vaddr + *folded]);
        vaddr = address;
        folded.reset();
    }
    const auto wrapped_fn = FastmemFallback(FallbackKind::Write, FallbackKey(ordered, bitsize, vaddr.getIdx(), value_idx));

    SharedLabel abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();

    if (fastmem_marker) {
        // Use fastmem
        bool require_abort_handling = false;
        const auto dest_ptr = EmitFastmemVAddr(code, ctx, *abort, vaddr, require_abort_handling);

        const auto location = EmitWriteMemoryMov<bitsize>(code, folded ? dest_ptr + *folded : dest_ptr, value_idx, ordered);

        ctx.deferred_emits.emplace_back([=, this, &ctx] {
            EmitFastmemFallback(ctx, inst, *abort, *end, vaddr, folded, wrapped_fn, location, *fastmem_marker);
        });
    } else {
        // Use page table
        ASSERT(conf.page_table);
        const auto dest_ptr = EmitVAddrLookup(code, ctx, bitsize, *abort, vaddr);
        EmitWriteMemoryMov<bitsize>(code, dest_ptr, value_idx, ordered);

        ctx.deferred_emits.emplace_back([=, this, &ctx] {
            code.L(*abort);
            code.call(wrapped_fn);
            EmitCheckMemoryAbort(ctx, inst, end);
            code.jmp(*end, code.T_NEAR);
        });
    }
    code.L(*end);
    if (ordered && bitsize == 128) code.ReloadA64CachedRbx();
}

template<std::size_t bitsize, auto callback>
void AxxEmitX64::EmitExclusiveReadMemory(AxxEmitContext& ctx, IR::Inst* inst) {
    ASSERT(conf.global_monitor != nullptr);
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool ordered = IsOrdered(args[2].GetImmediateAccType());

    if constexpr (bitsize != 128) {
        using T = mcl::unsigned_integer_of_size<bitsize>;

        ctx.reg_alloc.HostCall(code, inst, {}, args[1]);

        code.mov(code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)], u8(1));
        code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(&conf));
        if (ordered) {
            code.mfence();
        }
        code.CallLambda([](AxxUserConfig& conf, Axx::VAddr vaddr) -> T {
            return conf.global_monitor->ReadAndMark<T>(conf.processor_id, vaddr, [&]() -> T {
                return (conf.callbacks->*callback)(vaddr, bitsize / CHAR_BIT);
            });
        });
        code.ZeroExtendFrom(bitsize, code.ABI_RETURN);
    } else {
        const Xbyak::Xmm result = ctx.reg_alloc.ScratchXmm(code);
        ctx.reg_alloc.Use(code, args[1], ABI_PARAM2);
        ctx.reg_alloc.EndOfAllocScope();
        ctx.reg_alloc.HostCall(code, nullptr);

        code.mov(code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)], u8(1));
        code.mov(code.ABI_PARAM1, reinterpret_cast<u64>(&conf));
        ctx.reg_alloc.AllocStackSpace(code, 16 + ABI_SHADOW_SPACE);
        code.lea(code.ABI_PARAM3, ptr[rsp + ABI_SHADOW_SPACE]);
        if (ordered) {
            code.mfence();
        }
        code.CallLambda([](AxxUserConfig& conf, Axx::VAddr vaddr, Vector& ret) {
            ret = conf.global_monitor->ReadAndMark<Vector>(conf.processor_id, vaddr, [&]() -> Vector {
                return (conf.callbacks->*callback)(vaddr);
            });
        });
        code.movups(result, xword[rsp + ABI_SHADOW_SPACE]);
        ctx.reg_alloc.ReleaseStackSpace(code, 16 + ABI_SHADOW_SPACE);

        ctx.reg_alloc.DefineValue(code, inst, result);
    }

    EmitCheckMemoryAbort(ctx, inst);
}

template<std::size_t bitsize, auto callback>
void AxxEmitX64::EmitExclusiveWriteMemory(AxxEmitContext& ctx, IR::Inst* inst) {
    ASSERT(conf.global_monitor != nullptr);
    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    const bool ordered = IsOrdered(args[3].GetImmediateAccType());

    if constexpr (bitsize == 128) {
        ctx.reg_alloc.Use(code, args[1], ABI_PARAM2);
        ctx.reg_alloc.Use(code, args[2], HostLoc::XMM1);
        ctx.reg_alloc.EndOfAllocScope();
        ctx.reg_alloc.HostCall(code, inst);
    } else {
        ctx.reg_alloc.HostCall(code, inst, {}, args[1], args[2]);
    }

    const Xbyak::Reg64 tmp = ctx.reg_alloc.ScratchGpr(code);
    Xbyak::Label end;
    code.mov(code.ABI_RETURN, u32(1));
    code.movzx(tmp.cvt32(), code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)]);
    code.test(tmp.cvt8(), tmp.cvt8());
    code.je(end);
    code.xor_(tmp.cvt32(), tmp.cvt32());
    code.xchg(tmp.cvt8(), code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)]);
    code.mov(code.ABI_PARAM1, u64(&conf));
    if constexpr (bitsize != 128) {
        using T = mcl::unsigned_integer_of_size<bitsize>;
        code.CallLambda([](AxxUserConfig& conf, Axx::VAddr vaddr, T value) -> u32 {
            return conf.global_monitor->DoExclusiveOperation<T>(conf.processor_id, vaddr, [&](T expected) -> bool {
                return (conf.callbacks->*callback)(vaddr, value, expected);
            }) ? 0 : 1;
        });
        if (ordered) {
            code.mfence();
        }
    } else {
        ctx.reg_alloc.AllocStackSpace(code, 16 + ABI_SHADOW_SPACE);
        code.lea(code.ABI_PARAM3, ptr[rsp + ABI_SHADOW_SPACE]);
        code.movaps(xword[code.ABI_PARAM3], xmm1);
        code.CallLambda([](AxxUserConfig& conf, Axx::VAddr vaddr, Vector& value) -> u32 {
            return conf.global_monitor->DoExclusiveOperation<Vector>(conf.processor_id, vaddr, [&](Vector expected) -> bool {
                return (conf.callbacks->*callback)(vaddr, value, expected);
            }) ? 0 : 1;
        });
        if (ordered) {
            code.mfence();
        }
        ctx.reg_alloc.ReleaseStackSpace(code, 16 + ABI_SHADOW_SPACE);
    }
    code.L(end);

    EmitCheckMemoryAbort(ctx, inst);
}

template<std::size_t bitsize, auto callback>
void AxxEmitX64::EmitExclusiveReadMemoryInline(AxxEmitContext& ctx, IR::Inst* inst) {
    ASSERT(conf.global_monitor && conf.fastmem_pointer);
    if (!exception_handler.SupportsFastmem()) {
        EmitExclusiveReadMemory<bitsize, callback>(ctx, inst);
        return;
    }

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    constexpr bool ordered = true;

    if constexpr (ordered && bitsize == 128) {
        // Required for atomic 128-bit loads/stores
        code.FlushA64CachedRbx();
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RBX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RCX);
        ctx.reg_alloc.ScratchGpr(code, HostLoc::RDX);
    }

    const Xbyak::Reg64 vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
    const int value_idx = bitsize == 128 ? ctx.reg_alloc.ScratchXmm(code).getIdx() : ctx.reg_alloc.ScratchGpr(code).getIdx();
    // Venture: the 128-bit form's temporaries are RCX and RDX, which its
    // CMPXCHG16B overwrites only between their uses (fewer GPRs beside the
    // register cache's).
    const Xbyak::Reg64 tmp = bitsize == 128 ? rcx : ctx.reg_alloc.ScratchGpr(code);
    const Xbyak::Reg64 tmp2 = bitsize == 128 ? rdx : ctx.reg_alloc.ScratchGpr(code);

    const auto wrapped_fn = FastmemFallback(FallbackKind::Read, FallbackKey(ordered, bitsize, vaddr.getIdx(), value_idx));

    EmitExclusiveLock(code, conf, tmp, tmp2.cvt32());

    code.mov(code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)], u8(1));
    code.mov(tmp, std::bit_cast<u64>(GetExclusiveMonitorAddressPointer(conf.global_monitor, conf.processor_id)));
    code.mov(qword[tmp], vaddr);
    if (const auto generations = GetExclusiveMonitorGenerationBase(conf.global_monitor)) {
        // Record the granule's generation before loading its value.
        EmitExclusiveGenerationBucket(code, generations, vaddr, tmp, tmp2);
        code.mov(tmp, qword[tmp]);
        code.mov(tmp2, std::bit_cast<u64>(GetExclusiveMonitorGenerationSlot(conf.global_monitor, conf.processor_id)));
        code.mov(qword[tmp2], tmp);
    }

    const auto fastmem_marker = ShouldFastmem(ctx, inst);
    if (fastmem_marker) {
        SharedLabel abort = ctx.GenSharedLabel(), end = ctx.GenSharedLabel();
        bool require_abort_handling = false;

        const auto src_ptr = EmitFastmemVAddr(code, ctx, *abort, vaddr, require_abort_handling);

        // Venture: hosts with AVX make 16-byte aligned VMOVDQA loads single-copy atomic
        // (Intel SDM 9.1.1; AMD likewise), and an exclusive pair is 16-byte aligned, so
        // its load is plain like the narrower ones: LDXP/STXP read idioms on shared
        // lock-free queues no longer take the line exclusive. A misaligned pair faults
        // into the fallback, as ARM would raise an alignment fault.
        const auto location = bitsize == 128 && conf.plain_exclusive_loads && code.HasHostFeature(HostFeature::AVX)
                                  ? EmitAtomicLoad128(code, value_idx, src_ptr)
                                  : EmitReadMemoryMov<bitsize>(code, value_idx, src_ptr, ordered, !conf.plain_exclusive_loads);

        fastmem_patch_info.emplace(
            std::bit_cast<u64>(location),
            FastmemPatchInfo{
                std::bit_cast<u64>(code.getCurr()),
                std::bit_cast<u64>(wrapped_fn),
                *fastmem_marker,
                conf.recompile_on_exclusive_fastmem_failure,
            });

        code.L(*end);

        if (require_abort_handling) {
            ctx.deferred_emits.emplace_back([=, this] {
                code.L(*abort);
                code.call(wrapped_fn);
                code.jmp(*end, code.T_NEAR);
            });
        }
    } else {
        code.call(wrapped_fn);
    }
    if constexpr (bitsize == 128) code.ReloadA64CachedRbx();

    code.mov(tmp, std::bit_cast<u64>(GetExclusiveMonitorValuePointer(conf.global_monitor, conf.processor_id)));
    EmitWriteMemoryMov<bitsize>(code, tmp, value_idx, false);

    EmitExclusiveUnlock(code, conf, tmp, tmp2.cvt32());

    if constexpr (bitsize == 128) {
        ctx.reg_alloc.DefineValue(code, inst, Xbyak::Xmm{value_idx});
    } else {
        ctx.reg_alloc.DefineValue(code, inst, Xbyak::Reg64{value_idx});
    }

    EmitCheckMemoryAbort(ctx, inst);
}

template<std::size_t bitsize, auto callback>
void AxxEmitX64::EmitExclusiveWriteMemoryInline(AxxEmitContext& ctx, IR::Inst* inst) {
    ASSERT(conf.global_monitor && conf.fastmem_pointer);
    if (!exception_handler.SupportsFastmem()) {
        EmitExclusiveWriteMemory<bitsize, callback>(ctx, inst);
        return;
    }

    auto args = ctx.reg_alloc.GetArgumentInfo(inst);
    constexpr bool ordered = true;

    const auto value = [&] {
        if constexpr (bitsize == 128) {
            code.FlushA64CachedRbx();
            ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
            ctx.reg_alloc.ScratchGpr(code, HostLoc::RBX);
            ctx.reg_alloc.ScratchGpr(code, HostLoc::RCX);
            ctx.reg_alloc.ScratchGpr(code, HostLoc::RDX);
            return ctx.reg_alloc.UseXmm(code, args[2]);
        } else {
            ctx.reg_alloc.ScratchGpr(code, HostLoc::RAX);
            return ctx.reg_alloc.UseGpr(code, args[2]);
        }
    }();
    const Xbyak::Reg64 vaddr = ctx.reg_alloc.UseGpr(code, args[1]);
    // Venture: fewer GPRs beside the register cache's. The 128-bit form's
    // temporaries are RDX and RCX, which its CMPXCHG16B operands overwrite after
    // their last use; the narrower forms keep the status in tmp2, which is set
    // again where tmp2's scratch uses end.
    const Xbyak::Reg64 tmp = bitsize == 128 ? rdx : ctx.reg_alloc.ScratchGpr(code);
    const Xbyak::Reg64 tmp2 = bitsize == 128 ? rcx : ctx.reg_alloc.ScratchGpr(code);
    const Xbyak::Reg32 status = bitsize == 128 ? ctx.reg_alloc.ScratchGpr(code).cvt32() : tmp2.cvt32();
    const bool status_in_tmp2 = status.getIdx() == tmp2.getIdx();
    // The 128-bit elision compares in an XMM register (PTEST); its load is atomic,
    // locked or VMOVDQA.
    const bool elide = conf.elide_unchanged_exclusive_stores && !IsOrdered(args[3].GetImmediateAccType()) &&
                       (bitsize <= 64 || code.HasHostFeature(HostFeature::SSE41)) && ExclusivePairIsAdjacent(ctx.block, inst);
    const auto compare = [&] {
        if constexpr (bitsize == 128) return elide ? ctx.reg_alloc.ScratchXmm(code) : Xbyak::Xmm{0};
        else return 0;
    }();

    const auto wrapped_fn = FastmemFallback(FallbackKind::ExclusiveWrite, FallbackKey(ordered, bitsize, vaddr.getIdx(), value.getIdx()));

    EmitExclusiveLock(code, conf, tmp, tmp2.cvt32());

    SharedLabel end = ctx.GenSharedLabel();

    code.mov(status, u32(1));
    code.movzx(tmp.cvt32(), code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)]);
    code.test(tmp.cvt8(), tmp.cvt8());
    code.je(*end, code.T_NEAR);
    code.mov(tmp, std::bit_cast<u64>(GetExclusiveMonitorAddressPointer(conf.global_monitor, conf.processor_id)));
    code.cmp(qword[tmp], vaddr);
    code.jne(*end, code.T_NEAR);

    // Venture: a plain (non-release) store-exclusive of the value its load
    // returned, with only register operations since that load, succeeds
    // without writing. The pair then linearizes at the single-copy atomic
    // load: nothing else was observed in between and the store would not
    // change memory. Idempotent updates (a bitmap OR whose bit is already
    // set) and LDXP/STXP atomic 128-bit reads skip a locked compare-exchange
    // and its store-buffer drain.
    if constexpr (bitsize == 128) {
        if (elide) {
            Xbyak::Label store;
            code.mov(tmp, std::bit_cast<u64>(GetExclusiveMonitorValuePointer(conf.global_monitor, conf.processor_id)));
            code.movups(compare, xword[tmp]);
            code.pxor(compare, value);
            code.ptest(compare, compare);
            code.jnz(store);
            code.mov(code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)], u8(0));
            code.xor_(status, status);
            code.jmp(*end, code.T_NEAR);
            code.L(store);
        }
    } else {
        (void)compare;
        if (elide) {
            Xbyak::Label store;
            code.mov(tmp, std::bit_cast<u64>(GetExclusiveMonitorValuePointer(conf.global_monitor, conf.processor_id)));
            EmitReadMemoryMov<bitsize>(code, tmp2.getIdx(), tmp, false);
            switch (bitsize) {
            case 8:
                code.cmp(value.cvt8(), tmp2.cvt8());
                break;
            case 16:
                code.cmp(value.cvt16(), tmp2.cvt16());
                break;
            case 32:
                code.cmp(value.cvt32(), tmp2.cvt32());
                break;
            default:
                code.cmp(value.cvt64(), tmp2);
                break;
            }
            code.jne(store);
            code.mov(code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)], u8(0));
            code.xor_(status, status);
            code.jmp(*end, code.T_NEAR);
            code.L(store);
        }
    }

    if (const auto generations = GetExclusiveMonitorGenerationBase(conf.global_monitor); generations && !ExclusivePairIsLocal(ctx.block, inst)) {
        // Claim the granule: advance its generation from the value the load
        // recorded, or fail because another exclusive store succeeded since.
        EmitExclusiveGenerationBucket(code, generations, vaddr, tmp2, tmp);
        code.mov(tmp, std::bit_cast<u64>(GetExclusiveMonitorGenerationSlot(conf.global_monitor, conf.processor_id)));
        code.mov(rax, qword[tmp]);
        code.lea(tmp, ptr[rax + 2]);
        code.lock();
        code.cmpxchg(qword[tmp2], tmp);
        if (status_in_tmp2) {
            SharedLabel failed = ctx.GenSharedLabel();
            code.jne(*failed, code.T_NEAR);
            ctx.deferred_emits.emplace_back([=, this] {
                code.L(*failed);
                code.mov(status, u32(1));
                code.jmp(*end, code.T_NEAR);
            });
        } else {
            code.jne(*end, code.T_NEAR);
        }
    }
    if (status_in_tmp2) {
        code.mov(status, u32(1));
    }
    EmitExclusiveTestAndClear(code, conf, vaddr, tmp, rax);

    code.mov(code.byte[code.ABI_JIT_PTR + offsetof(AxxJitState, exclusive_state)], u8(0));
    code.mov(tmp, std::bit_cast<u64>(GetExclusiveMonitorValuePointer(conf.global_monitor, conf.processor_id)));

    if constexpr (bitsize == 128) {
        code.mov(rax, qword[tmp + 0]);
        code.mov(rdx, qword[tmp + 8]);
        if (code.HasHostFeature(HostFeature::SSE41)) {
            code.movq(rbx, value);
            code.pextrq(rcx, value, 1);
        } else {
            code.movaps(xmm0, value);
            code.movq(rbx, xmm0);
            code.punpckhqdq(xmm0, xmm0);
            code.movq(rcx, xmm0);
        }
    } else {
        EmitReadMemoryMov<bitsize>(code, rax.getIdx(), tmp, false);
    }

    const auto fastmem_marker = ShouldFastmem(ctx, inst);
    if (fastmem_marker) {
        SharedLabel abort = ctx.GenSharedLabel();
        bool require_abort_handling = false;

        // RDX holds the 128-bit form's expected value here.
        const auto dest_ptr = EmitFastmemVAddr(code, ctx, *abort, vaddr, require_abort_handling, bitsize == 128 ? std::nullopt : std::optional{tmp});

        const auto location = code.getCurr();

        switch (bitsize) {
        case 8:
            code.lock();
            code.cmpxchg(code.byte[dest_ptr], value.cvt8());
            break;
        case 16:
            code.lock();
            code.cmpxchg(word[dest_ptr], value.cvt16());
            break;
        case 32:
            code.lock();
            code.cmpxchg(dword[dest_ptr], value.cvt32());
            break;
        case 64:
            code.lock();
            code.cmpxchg(qword[dest_ptr], value.cvt64());
            break;
        case 128:
            code.lock();
            code.cmpxchg16b(ptr[dest_ptr]);
            break;
        default:
            UNREACHABLE();
        }

        code.setnz(status.cvt8());
        ctx.deferred_emits.emplace_back([=, this] {
            code.L(*abort);
            code.call(wrapped_fn);

            fastmem_patch_info.emplace(
                std::bit_cast<u64>(location),
                FastmemPatchInfo{
                    std::bit_cast<u64>(code.getCurr()),
                    std::bit_cast<u64>(wrapped_fn),
                    *fastmem_marker,
                    conf.recompile_on_exclusive_fastmem_failure,
                });

            code.xor_(status.cvt32(), status.cvt32()); //dep-break
            code.test(code.al, code.al);
            code.setz(status.cvt8());
            code.jmp(*end, code.T_NEAR);
        });
    } else {
        code.call(wrapped_fn);
        code.xor_(status.cvt32(), status.cvt32()); //dep-break
        code.test(code.al, code.al);
        code.setz(status.cvt8());
    }

    code.L(*end);
    if constexpr (bitsize == 128) code.ReloadA64CachedRbx();
    EmitExclusiveUnlock(code, conf, tmp, eax);
    ctx.reg_alloc.DefineValue(code, inst, status);
    EmitCheckMemoryAbort(ctx, inst);
}

#undef AxxEmitX64
#undef AxxEmitContext
#undef AxxJitState
#undef AxxUserConfig
