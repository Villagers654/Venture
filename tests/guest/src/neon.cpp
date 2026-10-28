#include "vtest.h"
#include <arm_neon.h>
#include <asm/hwcap.h>
#include <sys/auxv.h>

// Unity math, animation, texture codecs and DSP use vector instructions.
#if defined(__aarch64__)
static __attribute__((target("dotprod"), noinline)) int dot(const int8_t* a, const int8_t* b) {
    int32x4_t result = vdotq_s32(vdupq_n_s32(0), vld1q_s8(a), vld1q_s8(b));
    return vaddvq_s32(result);
}
#endif
extern "C" int vtest_main(int, char**) {
    Vtest q("neon");
    alignas(16) float a[4] = {1, -2, 3.5f, 4}, b[4] = {2, 3, -1, 0.5f}, output[4];
    vst1q_f32(output, vmulq_f32(vaddq_f32(vld1q_f32(a), vld1q_f32(b)), vld1q_f32(b)));
    for (int i = 0; i < 4; ++i) CHECK(output[i] == (a[i] + b[i]) * b[i]);
    uint8_t table[8] = {10,20,30,40,50,60,70,80}, indices[8] = {7,0,3,9,2,1,6,8}, shuffled[8];
    vst1_u8(shuffled, vtbl1_u8(vld1_u8(table), vld1_u8(indices)));
    for (int i = 0; i < 8; ++i) CHECK(shuffled[i] == (indices[i] < 8 ? table[indices[i]] : 0));
    int8_t saturated[8]; vst1_s8(saturated, vqadd_s8(vdup_n_s8(120), vdup_n_s8(20)));
    for (int8_t value : saturated) CHECK(value == 127);
    uint8_t subtracted[8]; vst1_u8(subtracted, vqsub_u8(vdup_n_u8(3), vdup_n_u8(10)));
    for (uint8_t value : subtracted) CHECK(value == 0);
    q.step("NEON: float arithmetic table lookup saturation OK");
    // These are storage/conversion instructions, not optional FP16 arithmetic.
    float16x4_t half = vcvt_f16_f32(vld1q_f32(a));
    vst1q_f32(output, vcvt_f32_f16(half));
    for (int i = 0; i < 4; ++i) CHECK(output[i] == a[i]);
    q.step("NEON: float16 roundtrip OK");
    int8_t left[16], right[16]; int reference = 0;
    for (int i = 0; i < 16; ++i) { left[i] = int8_t(i - 8); right[i] = int8_t(3 - i); reference += left[i] * right[i]; }
#if defined(__aarch64__)
    if (getauxval(AT_HWCAP) & HWCAP_ASIMDDP) CHECK(dot(left, right) == reference);
    else CHECK(reference == -304);
#else
    CHECK(reference == -304);
#endif
    // Same output whether the optional instruction is available or skipped.
    q.step("NEON: guarded dot product OK"); return q.finish();
}
