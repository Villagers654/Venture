// Original CPU-only EHABI ABI fixture. The opaque register-set layout is
// deliberately different from GCC and LLVM; Venture must use the public APIs.
#include <stdint.h>
#include <unwind.h>

struct _Unwind_Context {
  uint32_t private_words[11];
  uint32_t registers[16];
  uint32_t gets, sets;
};
_Unwind_VRS_Result _Unwind_VRS_Get(struct _Unwind_Context* c, _Unwind_VRS_RegClass kind,
    uint32_t reg, _Unwind_VRS_DataRepresentation representation, void* out) {
  if (kind != _UVRSC_CORE || representation != _UVRSD_UINT32 || reg > 15) return _UVRSR_FAILED;
  ++c->gets; *(uint32_t*)out = c->registers[reg]; return _UVRSR_OK;
}
_Unwind_VRS_Result _Unwind_VRS_Set(struct _Unwind_Context* c, _Unwind_VRS_RegClass kind,
    uint32_t reg, _Unwind_VRS_DataRepresentation representation, void* in) {
  if (kind != _UVRSC_CORE || representation != _UVRSD_UINT32 || reg > 15) return _UVRSR_FAILED;
  ++c->sets; c->registers[reg] = *(uint32_t*)in; return _UVRSR_OK;
}
extern uintptr_t dl_unwind_find_exidx(uintptr_t, int*);
static uintptr_t relative(uint32_t* field) { return (uintptr_t)field + ((int32_t)(*field << 1) >> 1); }
uint32_t condition_personality_fixture(uint32_t pc, uint32_t sp, uint32_t phase, uint32_t* out) {
  int count = 0;
  uint32_t* table = (uint32_t*)dl_unwind_find_exidx(pc, &count);
  uint32_t* row = 0;
  for (int i = 0; i < count; ++i) {
    if (relative(table + i*2) > pc) break;
    row = table + i*2;
  }
  if (!row || row[1] == 1 || (row[1] >> 31)) return 77;
  uint32_t* extab = (uint32_t*)relative(row + 1);
  typedef _Unwind_Reason_Code (*Personality32)(_Unwind_State, _Unwind_Exception*, struct _Unwind_Context*);
  Personality32 personality = (Personality32)relative(extab);
  struct _Unwind_Context context = {0};
  context.private_words[0] = 0x12345678;
  context.registers[13] = sp;
  _Unwind_Exception exception = {0};
  out[0] = personality(phase, &exception, &context);
  out[1] = context.registers[4]; out[2] = context.registers[13]; out[3] = context.registers[15];
  out[4] = context.gets; out[5] = context.sets; out[6] = context.private_words[0];
  return 0;
}
