/* Byte offsets of NativeContext (cpu/native_context.h) for the host entry
 * routines in cpu/native_entry_a64.S. Plain #defines so the assembler can use
 * them; native_context.h checks every one against the struct. */
#ifndef VENTURE_CPU_NATIVE_CONTEXT_OFFSETS_H
#define VENTURE_CPU_NATIVE_CONTEXT_OFFSETS_H

/* Guest state. */
#define VENTURE_NCTX_X 0              /* x0..x30, 8 bytes each */
#define VENTURE_NCTX_X16 128
#define VENTURE_NCTX_X18 144
#define VENTURE_NCTX_X19 152
#define VENTURE_NCTX_X20 160
#define VENTURE_NCTX_X29 232
#define VENTURE_NCTX_SP 248
#define VENTURE_NCTX_PC 256
#define VENTURE_NCTX_TPIDR 264
#define VENTURE_NCTX_NZCV 272         /* 32-bit */
#define VENTURE_NCTX_FPCR 276         /* 32-bit */
#define VENTURE_NCTX_FPSR 280         /* 32-bit */
#define VENTURE_NCTX_KIND 284         /* 32-bit: SVC immediate of the call kind */
#define VENTURE_NCTX_Q 288            /* q0..q31, 16 bytes each */
#define VENTURE_NCTX_Q8 416
/* Host state, written by venture_native_enter (Linux). */
#define VENTURE_NCTX_HOST_TPIDR 800
#define VENTURE_NCTX_HOST_SP 808
#define VENTURE_NCTX_HOST_X18 816
#define VENTURE_NCTX_HOST_FPCR 824
#define VENTURE_NCTX_HOST_X19 832     /* x19..x30, 12 x 8 bytes */
#define VENTURE_NCTX_HOST_D8 928      /* d8..d15, 8 x 8 bytes */
/* Exit and control words. */
#define VENTURE_NCTX_EXIT_REASON 992  /* 32-bit, VENTURE_NATIVE_EXIT_* */
#define VENTURE_NCTX_SVC_IMMEDIATE 996
#define VENTURE_NCTX_HALT 1000        /* 32-bit, nonzero: leave run() soon */
#define VENTURE_NCTX_FLAGS 1004       /* 32-bit, VENTURE_NATIVE_FLAG_* */
#define VENTURE_NCTX_OWNER 1008
#define VENTURE_NCTX_SIZE 1024

/* Exit reasons: what venture_native_enter and the dispatcher return. */
#define VENTURE_NATIVE_EXIT_NONE 0        /* the handler completed; back to guest code */
#define VENTURE_NATIVE_EXIT_SVC 1         /* CpuExit::Svc, svc_immediate says which */
#define VENTURE_NATIVE_EXIT_HALT 2        /* CpuExit::HaltRequested */
#define VENTURE_NATIVE_EXIT_HLE_EXCEPTION 3 /* CpuExit::HleException */

/* Flags. */
#define VENTURE_NATIVE_FLAG_SHARED_TP 1   /* guest TPIDR_EL0/x18 are the host's: no switch */

/* Where the per-thread context pointer sits on Linux: [TPIDR_EL0 - 16]. */
#define VENTURE_NATIVE_TP_CONTEXT_OFFSET 16

#endif
