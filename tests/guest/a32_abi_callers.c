// Compile in ARM and Thumb modes with the NDK r30 ARMv7 softfp target.
// External sinks are intercepted by the oracle runner, before a callee prologue.
#include <stdint.h>
#include <stddef.h>
struct split { uint32_t a, b, c; };
struct result { uint32_t a, b, c; };
extern void sink_mixed(int32_t, uint64_t, float, double, size_t);
extern void sink_skip(int32_t, int32_t, int32_t, uint64_t, int32_t, double);
extern void sink_split(int32_t, int32_t, struct split, uint32_t);
extern void sink_narrow(int8_t, uint8_t, int16_t, uint16_t);
extern void sink_varargs(int32_t, ...);
extern struct result sink_sret(int32_t, double);
extern void sink_result(struct result);
void call_mixed(void) { sink_mixed(-7, UINT64_C(0x1122334455667788), 1.25f, -3.5, (size_t)0xfedcba98); }
void call_skip(void) { sink_skip(11, 22, 33, UINT64_C(0x1234567887654321), -44, 9.25); }
void call_split(void) { sink_split(7, 8, (struct split){11, 22, 33}, 44); }
void call_narrow(void) { sink_narrow(-1, 255, -2, 65535); }
void call_varargs(void) { sink_varargs(4, -7, UINT64_C(0xfedcba9876543210), 1.25f, (void*)0x34567890); }
void call_sret(void) { sink_result(sink_sret(77, 2.5)); }
