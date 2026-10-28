// Legal A64 templates; every base encoding was checked with the clang
// integrated assembler (aarch64) and llvm-objdump. D/N/M/A register fields are
// independently randomized. GPR fields exclude the reserved loop counter (w27)
// and memory base (x28); lane indices and conditions remain legal for any value.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace jit_ir_equivalence {
// Template::fields bits.
enum Field : unsigned {
    kRd = 1,               // bits 0-4
    kRn = 2,               // bits 5-9
    kRm = 4,               // bits 16-20
    kRa = 8,               // bits 10-14 (Ra, or Rt2 for pairs)
    kCond = 16,            // bits 12-15, ORed into a base condition of 0000
    kImm12Small = 32,      // ADD/SUB imm12 replaced by 0..31
    kMemImm12 = 64,        // scaled unsigned offset, a multiple of 16 bytes below 480
    kMovVectorAlias = 128, // ORR Rm = Rn, the MOV vector alias
    kImm16Small = 256,     // MOVZ imm16 replaced by 0..7
    kPairImm7 = 512,       // pair offset inside the 512-byte fixture
    kDistinctRtRt2 = 1024, // LDP with Rt2 == Rt is CONSTRAINED UNPREDICTABLE
    kImm9 = 2048,          // unscaled LDUR/STUR offset 0..255
    kNzcvImm = 4096,       // CCMP/FCCMP immediate flags in bits 0-3
    kBasePrefix = 8192,    // base x26, set by a fused "add x26, x28, #offset"
};
struct Template {
    std::uint32_t word;
    unsigned fields, gpr_fields, category, memory_size; // memory_size: bytes accessed
    const char* assembly;
    // kBasePrefix templates: the access starts at x26 + access_offset, and an
    // immediate writeback adds writeback to x26 (0: none or a register amount).
    int access_offset = 0, writeback = 0;
};
inline constexpr std::array<const char*, 9> categories{
    "scalar-fp", "vector-fp", "lane-move", "convert", "memory", "integer", "flags", "constant", "memory-multi"};
// Category weights used by the generator (percent). FP and lane traffic dominate.
inline constexpr std::array<unsigned, 9> category_weights{16, 16, 16, 8, 16, 10, 6, 4, 8};
inline constexpr Template instructions[] = {
    // Scalar FP.
    {0x1e202800, 7, 0, 0, 16, "fadd s0, s0, s0"},
    {0x1e200800, 7, 0, 0, 16, "fmul s0, s0, s0"},
    {0x1e203800, 7, 0, 0, 16, "fsub s0, s0, s0"},
    {0x1e201800, 7, 0, 0, 16, "fdiv s0, s0, s0"},
    {0x1f000000, 15, 0, 0, 16, "fmadd s0, s0, s0, s0"},
    {0x1e204000, 3, 0, 2, 16, "fmov s0, s0"},
    {0x1e214000, 3, 0, 0, 16, "fneg s0, s0"},
    {0x1e20c000, 3, 0, 0, 16, "fabs s0, s0"},
    {0x1e21c000, 3, 0, 0, 16, "fsqrt s0, s0"},
    {0x1e602800, 7, 0, 0, 16, "fadd d0, d0, d0"},
    {0x1e600800, 7, 0, 0, 16, "fmul d0, d0, d0"},
    {0x1e603800, 7, 0, 0, 16, "fsub d0, d0, d0"},
    {0x1e601800, 7, 0, 0, 16, "fdiv d0, d0, d0"},
    {0x1f400000, 15, 0, 0, 16, "fmadd d0, d0, d0, d0"},
    {0x1e604000, 3, 0, 2, 16, "fmov d0, d0"},
    {0x1e614000, 3, 0, 0, 16, "fneg d0, d0"},
    {0x1e60c000, 3, 0, 0, 16, "fabs d0, d0"},
    {0x1e61c000, 3, 0, 0, 16, "fsqrt d0, d0"},
    {0x5fa09800, 7, 0, 0, 16, "fmul s0, s0, v0.s[3]"},
    {0x5fc01800, 7, 0, 0, 16, "fmla d0, d0, v0.d[1]"},
    {0x1e606800, 7, 0, 0, 16, "fmaxnm d0, d0, d0"},
    {0x1e207800, 7, 0, 0, 16, "fminnm s0, s0, s0"},
    {0x1e208800, 7, 0, 0, 16, "fnmul s0, s0, s0"},
    {0x1f408000, 15, 0, 0, 16, "fmsub d0, d0, d0, d0"},
    {0x1e254000, 3, 0, 0, 16, "frintm s0, s0"},
    {0x7e30d800, 3, 0, 0, 16, "faddp s0, v0.2s"},
    // Vector FP and vector permutes.
    {0x0e20d400, 7, 0, 1, 16, "fadd v0.2s, v0.2s, v0.2s"},
    {0x2e20dc00, 7, 0, 1, 16, "fmul v0.2s, v0.2s, v0.2s"},
    {0x0e20cc00, 7, 0, 1, 16, "fmla v0.2s, v0.2s, v0.2s"},
    {0x0ea0d400, 7, 0, 1, 16, "fsub v0.2s, v0.2s, v0.2s"},
    {0x4e20d400, 7, 0, 1, 16, "fadd v0.4s, v0.4s, v0.4s"},
    {0x6e20dc00, 7, 0, 1, 16, "fmul v0.4s, v0.4s, v0.4s"},
    {0x4e20cc00, 7, 0, 1, 16, "fmla v0.4s, v0.4s, v0.4s"},
    {0x4ea0d400, 7, 0, 1, 16, "fsub v0.4s, v0.4s, v0.4s"},
    {0x4e60d400, 7, 0, 1, 16, "fadd v0.2d, v0.2d, v0.2d"},
    {0x6e60dc00, 7, 0, 1, 16, "fmul v0.2d, v0.2d, v0.2d"},
    {0x4e60cc00, 7, 0, 1, 16, "fmla v0.2d, v0.2d, v0.2d"},
    {0x4ee0d400, 7, 0, 1, 16, "fsub v0.2d, v0.2d, v0.2d"},
    {0x4fa01000, 7, 0, 1, 16, "fmla v0.4s, v0.4s, v0.s[1]"},
    {0x0fa09800, 7, 0, 1, 16, "fmul v0.2s, v0.2s, v0.s[3]"},
    {0x4fc01800, 7, 0, 1, 16, "fmla v0.2d, v0.2d, v0.d[1]"},
    {0x6e20e400, 7, 0, 1, 16, "fcmge v0.4s, v0.4s, v0.4s"},
    {0x4e20c400, 7, 0, 1, 16, "fmaxnm v0.4s, v0.4s, v0.4s"},
    {0x6e20d400, 7, 0, 1, 16, "faddp v0.4s, v0.4s, v0.4s"},
    {0x6e601c00, 7, 0, 1, 16, "bsl v0.16b, v0.16b, v0.16b"},
    {0x6e004000, 7, 0, 1, 16, "ext v0.16b, v0.16b, v0.16b, #8"},
    {0x4e803800, 7, 0, 1, 16, "zip1 v0.4s, v0.4s, v0.4s"},
    {0x4ea12800, 3, 0, 1, 16, "xtn2 v0.4s, v0.2d"},
    // 64-bit vector forms whose upper halves the backend tracks as zero.
    {0x2e002000, 7, 0, 1, 16, "ext v0.8b, v0.8b, v0.8b, #4"},
    {0x0e212800, 3, 0, 1, 16, "xtn v0.8b, v0.8h"},
    {0x0e612800, 3, 0, 1, 16, "xtn v0.4h, v0.4s"},
    {0x0ea12800, 3, 0, 1, 16, "xtn v0.2s, v0.2d"},
    {0x0ea00800, 3, 0, 1, 16, "rev64 v0.2s, v0.2s"},
    {0x0e201c00, 7, 0, 1, 16, "and v0.8b, v0.8b, v0.8b"},
    {0x0ea08400, 7, 0, 1, 16, "add v0.2s, v0.2s, v0.2s"},
    {0x0fa09000, 7, 0, 1, 16, "fmul v0.2s, v0.2s, v0.s[1]"},
    // Lane moves, inserts, duplicates and register aliases.
    {0x1e260000, 3, 1, 2, 16, "fmov w0, s0"},
    {0x1e270000, 3, 2, 2, 16, "fmov s0, w0"},
    {0x9e660000, 3, 1, 2, 16, "fmov x0, d0"},
    {0x9e670000, 3, 2, 2, 16, "fmov d0, x0"},
    {0x9eae0000, 3, 1, 2, 16, "fmov x0, v0.d[1]"},
    {0x9eaf0000, 3, 2, 2, 16, "fmov v0.d[1], x0"},
    {0x4e011c00, 3, 2, 2, 16, "ins v0.b[0], w0"},
    {0x0e013c00, 3, 1, 2, 16, "umov w0, v0.b[0]"},
    {0x4e010400, 3, 0, 2, 16, "dup v0.16b, v0.b[0]"},
    {0x6e017c00, 3, 0, 2, 16, "ins v0.b[0], v0.b[15]"},
    {0x4e1f1c00, 3, 2, 2, 16, "ins v0.b[15], w0"},
    {0x0e1f3c00, 3, 1, 2, 16, "umov w0, v0.b[15]"},
    {0x4e1f0400, 3, 0, 2, 16, "dup v0.16b, v0.b[15]"},
    {0x6e1f0400, 3, 0, 2, 16, "ins v0.b[15], v0.b[0]"},
    {0x4e010c00, 3, 2, 2, 16, "dup v0.16b, w0"},
    {0x4e021c00, 3, 2, 2, 16, "ins v0.h[0], w0"},
    {0x0e023c00, 3, 1, 2, 16, "umov w0, v0.h[0]"},
    {0x4e020400, 3, 0, 2, 16, "dup v0.8h, v0.h[0]"},
    {0x6e027400, 3, 0, 2, 16, "ins v0.h[0], v0.h[7]"},
    {0x4e1e1c00, 3, 2, 2, 16, "ins v0.h[7], w0"},
    {0x0e1e3c00, 3, 1, 2, 16, "umov w0, v0.h[7]"},
    {0x4e1e0400, 3, 0, 2, 16, "dup v0.8h, v0.h[7]"},
    {0x6e1e0400, 3, 0, 2, 16, "ins v0.h[7], v0.h[0]"},
    {0x4e020c00, 3, 2, 2, 16, "dup v0.8h, w0"},
    {0x4e041c00, 3, 2, 2, 16, "ins v0.s[0], w0"},
    {0x0e043c00, 3, 1, 2, 16, "umov w0, v0.s[0]"},
    {0x4e040400, 3, 0, 2, 16, "dup v0.4s, v0.s[0]"},
    {0x6e046400, 3, 0, 2, 16, "ins v0.s[0], v0.s[3]"},
    {0x4e1c1c00, 3, 2, 2, 16, "ins v0.s[3], w0"},
    {0x0e1c3c00, 3, 1, 2, 16, "umov w0, v0.s[3]"},
    {0x4e1c0400, 3, 0, 2, 16, "dup v0.4s, v0.s[3]"},
    {0x6e1c0400, 3, 0, 2, 16, "ins v0.s[3], v0.s[0]"},
    {0x4e040c00, 3, 2, 2, 16, "dup v0.4s, w0"},
    {0x6e0c4400, 3, 0, 2, 16, "ins v0.s[1], v0.s[2]"},
    {0x6e142400, 3, 0, 2, 16, "ins v0.s[2], v0.s[1]"},
    {0x4e081c00, 3, 2, 2, 16, "ins v0.d[0], x0"},
    {0x4e083c00, 3, 1, 2, 16, "umov x0, v0.d[0]"},
    {0x4e080400, 3, 0, 2, 16, "dup v0.2d, v0.d[0]"},
    {0x6e084400, 3, 0, 2, 16, "ins v0.d[0], v0.d[1]"},
    {0x4e181c00, 3, 2, 2, 16, "ins v0.d[1], x0"},
    {0x4e183c00, 3, 1, 2, 16, "umov x0, v0.d[1]"},
    {0x4e180400, 3, 0, 2, 16, "dup v0.2d, v0.d[1]"},
    {0x6e180400, 3, 0, 2, 16, "ins v0.d[1], v0.d[0]"},
    {0x4e080c00, 3, 2, 2, 16, "dup v0.2d, x0"},
    {0x0ea01c00, 131, 0, 2, 16, "mov v0.8b, v0.8b"},
    {0x4ea01c00, 131, 0, 2, 16, "mov v0.16b, v0.16b"},
    // Conversions, including partial-width narrowing and widening.
    {0x9e380000, 3, 1, 3, 16, "fcvtzs x0, s0"},
    {0x9e220000, 3, 2, 3, 16, "scvtf s0, x0"},
    {0x1e230000, 3, 2, 3, 16, "ucvtf s0, w0"},
    {0x9e780000, 3, 1, 3, 16, "fcvtzs x0, d0"},
    {0x9e620000, 3, 2, 3, 16, "scvtf d0, x0"},
    {0x1e630000, 3, 2, 3, 16, "ucvtf d0, w0"},
    {0x1e390000, 3, 1, 3, 16, "fcvtzu w0, s0"},
    {0x9e790000, 3, 1, 3, 16, "fcvtzu x0, d0"},
    {0x1e22c000, 3, 0, 3, 16, "fcvt d0, s0"},
    {0x1e624000, 3, 0, 3, 16, "fcvt s0, d0"},
    {0x1e23c000, 3, 0, 3, 16, "fcvt h0, s0"},
    {0x1ee24000, 3, 0, 3, 16, "fcvt s0, h0"},
    {0x1e63c000, 3, 0, 3, 16, "fcvt h0, d0"},
    {0x1ee2c000, 3, 0, 3, 16, "fcvt d0, h0"},
    {0x0e616800, 3, 0, 3, 16, "fcvtn v0.2s, v0.2d"},
    {0x4e616800, 3, 0, 3, 16, "fcvtn2 v0.4s, v0.2d"},
    {0x0e617800, 3, 0, 3, 16, "fcvtl v0.2d, v0.2s"},
    {0x4e617800, 3, 0, 3, 16, "fcvtl2 v0.2d, v0.4s"},
    {0x4ea1b800, 3, 0, 3, 16, "fcvtzs v0.4s, v0.4s"},
    {0x4e61d800, 3, 0, 3, 16, "scvtf v0.2d, v0.2d"},
    // Memory: B/H/S/D/Q single, pairs, unscaled and LD1/ST1 forms, all on x28.
    {0xbd400380, 65, 0, 4, 4, "ldr s0, [x28]"},
    {0xbd000380, 65, 0, 4, 4, "str s0, [x28]"},
    {0xfd400380, 65, 0, 4, 8, "ldr d0, [x28]"},
    {0xfd000380, 65, 0, 4, 8, "str d0, [x28]"},
    {0x3dc00380, 65, 0, 4, 16, "ldr q0, [x28]"},
    {0x3d800380, 65, 0, 4, 16, "str q0, [x28]"},
    {0x7d400380, 65, 0, 4, 2, "ldr h0, [x28]"},
    {0x7d000380, 65, 0, 4, 2, "str h0, [x28]"},
    {0x3d400380, 65, 0, 4, 1, "ldr b0, [x28]"},
    {0xad000380, 521, 0, 4, 16, "stp q0, q0, [x28]"},
    {0xad400380, 1545, 0, 4, 16, "ldp q0, q1, [x28]"},
    {0x6d000380, 521, 0, 4, 8, "stp d0, d0, [x28]"},
    {0x6d400380, 1545, 0, 4, 8, "ldp d0, d1, [x28]"},
    {0x2d000380, 521, 0, 4, 4, "stp s0, s0, [x28]"},
    {0x2d400380, 1545, 0, 4, 4, "ldp s0, s1, [x28]"},
    {0x3cc00380, 2049, 0, 4, 16, "ldur q0, [x28]"},
    {0xfc000380, 2049, 0, 4, 8, "stur d0, [x28]"},
    {0x4c407b80, 1, 0, 4, 16, "ld1 {v0.4s}, [x28]"},
    {0x4c007f80, 1, 0, 4, 16, "st1 {v0.2d}, [x28]"},
    {0x0d409380, 1, 0, 4, 4, "ld1 {v0.s}[1], [x28]"},
    {0x4d008780, 1, 0, 4, 8, "st1 {v0.d}[1], [x28]"},
    // Multi-register and writeback forms. Register lists wrap modulo 32; the
    // x26 base is re-established before each one, and register post-index
    // amounts are arbitrary because nothing else addresses through x26.
    {0x4c00af40, 8193, 0, 8, 32, "st1 {v0.2d, v1.2d}, [x26]"},
    {0x4c006b40, 8193, 0, 8, 48, "st1 {v0.4s-v2.4s}, [x26]"},
    {0x4c002340, 8193, 0, 8, 64, "st1 {v0.16b-v3.16b}, [x26]"},
    {0x4c40af40, 8193, 0, 8, 32, "ld1 {v0.2d, v1.2d}, [x26]"},
    {0x4c406b40, 8193, 0, 8, 48, "ld1 {v0.4s-v2.4s}, [x26]"},
    {0x4c402340, 8193, 0, 8, 64, "ld1 {v0.16b-v3.16b}, [x26]"},
    {0x4c9f2f40, 8193, 0, 8, 64, "st1 {v0.2d-v3.2d}, [x26], #64", 0, 64},
    {0x4cdfab40, 8193, 0, 8, 32, "ld1 {v0.4s, v1.4s}, [x26], #32", 0, 32},
    {0x4c9f6340, 8193, 0, 8, 48, "st1 {v0.16b-v2.16b}, [x26], #48", 0, 48},
    {0x4cdf2f40, 8193, 0, 8, 64, "ld1 {v0.2d-v3.2d}, [x26], #64", 0, 64},
    {0x4c802f40, 8197, 4, 8, 64, "st1 {v0.2d-v3.2d}, [x26], x0"},
    {0x4cc0a340, 8197, 4, 8, 32, "ld1 {v0.16b, v1.16b}, [x26], x0"},
    {0x4c008b40, 8193, 0, 8, 32, "st2 {v0.4s, v1.4s}, [x26]"},
    {0x4c408f40, 8193, 0, 8, 32, "ld2 {v0.2d, v1.2d}, [x26]"},
    {0x4c9f8f40, 8193, 0, 8, 32, "st2 {v0.2d, v1.2d}, [x26], #32", 0, 32},
    {0x4cdf8b40, 8193, 0, 8, 32, "ld2 {v0.4s, v1.4s}, [x26], #32", 0, 32},
    {0xad810740, 8201, 0, 8, 32, "stp q0, q1, [x26, #32]!", 32, 32},
    {0xac810740, 8201, 0, 8, 32, "stp q0, q1, [x26], #32", 0, 32},
    {0xadff0740, 9225, 0, 8, 32, "ldp q0, q1, [x26, #-32]!", -32, -32},
    {0xacc10740, 9225, 0, 8, 32, "ldp q0, q1, [x26], #32", 0, 32},
    {0x3c810f40, 8193, 0, 8, 16, "str q0, [x26, #16]!", 16, 16},
    {0x3cc10740, 8193, 0, 8, 16, "ldr q0, [x26], #16", 0, 16},
    {0x6d810740, 8201, 0, 8, 16, "stp d0, d1, [x26, #16]!", 16, 16},
    {0x6cc10740, 9225, 0, 8, 16, "ldp d0, d1, [x26], #16", 0, 16},
    {0x4c002f80, 1, 0, 8, 64, "st1 {v0.2d-v3.2d}, [x28]"},
    {0x4c402f80, 1, 0, 8, 64, "ld1 {v0.2d-v3.2d}, [x28]"},
    // Integer, including SP reads and writes.
    {0x0b000000, 7, 7, 5, 16, "add w0, w0, w0"},
    {0x8b000000, 7, 7, 5, 16, "add x0, x0, x0"},
    {0x4b000000, 7, 7, 5, 16, "sub w0, w0, w0"},
    {0xcb000000, 7, 7, 5, 16, "sub x0, x0, x0"},
    {0x2b000000, 7, 7, 5, 16, "adds w0, w0, w0"},
    {0xab000000, 7, 7, 5, 16, "adds x0, x0, x0"},
    {0x6b000000, 7, 7, 5, 16, "subs w0, w0, w0"},
    {0xeb000000, 7, 7, 5, 16, "subs x0, x0, x0"},
    {0xba000000, 7, 7, 5, 16, "adcs x0, x0, x0"},
    {0x0a000000, 7, 7, 5, 16, "and w0, w0, w0"},
    {0x8a000000, 7, 7, 5, 16, "and x0, x0, x0"},
    {0x2a000000, 7, 7, 5, 16, "orr w0, w0, w0"},
    {0xaa000000, 7, 7, 5, 16, "orr x0, x0, x0"},
    {0x4a000000, 7, 7, 5, 16, "eor w0, w0, w0"},
    {0xca000000, 7, 7, 5, 16, "eor x0, x0, x0"},
    // Bitfield moves (shift-and-mask IR), low-word masks and W-register operands.
    {0xd360fc00, 3, 3, 5, 16, "lsr x0, x0, #32"},
    {0x531c6c00, 3, 3, 5, 16, "lsl w0, w0, #4"},
    {0xd3485c00, 3, 3, 5, 16, "ubfx x0, x0, #8, #16"},
    {0xd37d4c00, 3, 3, 5, 16, "ubfiz x0, x0, #3, #20"},
    {0x93443c00, 3, 3, 5, 16, "sbfx x0, x0, #4, #12"},
    {0x131e2000, 3, 3, 5, 16, "sbfiz w0, w0, #2, #9"},
    {0x92407c00, 3, 3, 5, 16, "and x0, x0, #0xffffffff"},
    {0x8b20cc00, 7, 7, 5, 16, "add x0, x0, w0, sxtw #3"},
    {0x8b204800, 7, 7, 5, 16, "add x0, x0, w0, uxtw #2"},
    {0x91002000, 35, 3, 5, 16, "add x0, x0, #8"},
    {0x910003ff, 32, 0, 5, 16, "add sp, sp, #0"},
    {0x910003e0, 33, 1, 5, 16, "add x0, sp, #0"},
    // Flags: integer and FP compares, conditional compares and selects, NZCV moves.
    {0x1e202000, 6, 0, 6, 16, "fcmp s0, s0"},
    {0x1e200c00, 23, 0, 6, 16, "fcsel s0, s0, s0, eq"},
    {0x1e602000, 6, 0, 6, 16, "fcmp d0, d0"},
    {0x1e600c00, 23, 0, 6, 16, "fcsel d0, d0, d0, eq"},
    {0x1e602008, 2, 0, 6, 16, "fcmp d0, #0.0"},
    {0x1e200400, 4118, 0, 6, 16, "fccmp s0, s0, #0, eq"},
    {0x1e600400, 4118, 0, 6, 16, "fccmp d0, d0, #0, eq"},
    {0xfa400000, 4118, 6, 6, 16, "ccmp x0, x0, #0, eq"},
    {0x9a800000, 23, 7, 6, 16, "csel x0, x0, x0, eq"},
    {0x1a9f07e0, 17, 1, 6, 16, "cset w0, ne"},
    {0xd53b4200, 1, 1, 6, 16, "mrs x0, nzcv"},
    {0xd51b4200, 1, 1, 6, 16, "msr nzcv, x0"},
    // Constants.
    {0x1e2e1000, 1, 0, 7, 16, "fmov s0, #1.0"},
    {0x1e6e1000, 1, 0, 7, 16, "fmov d0, #1.0"},
    {0x6f00e400, 1, 0, 7, 16, "movi v0.2d, #0"},
    {0x2f00e400, 1, 0, 7, 16, "movi d0, #0"},
    {0x4f03f600, 1, 0, 7, 16, "fmov v0.4s, #1.0"},
    {0xd2800000, 257, 1, 7, 16, "mov x0, #0"},
    {0x52800020, 257, 1, 7, 16, "mov w0, #1"},
};
inline constexpr std::size_t instruction_count = std::size(instructions);
} // namespace jit_ir_equivalence
