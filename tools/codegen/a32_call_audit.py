#!/usr/bin/env python3
"""List host->guest calls and their A32 migration requirements (JSON, outside build).

The classification is a reviewed worklist, not a claim that a regex proves ABI
correctness. Zero-argument init/fini calls can keep the untyped word form.
"""
import argparse
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
REVIEW = {
 'main.cpp': 'vtest_main argv needs low strings and 4-byte pointer array; JNI_OnLoad needs JavaVM32 wrapper; jint result uses i32()',
 'core/elf.cpp': 'zero-argument initializers/finalizers can keep word form; loader owns 4-byte function arrays',
 'core/linker.cpp': 'A64 ifunc HWCAP bit 62 and host-local argument are invalid on A32; separate ARM resolver ABI',
 'core/thread.cpp': 'thread start argument must be a low pointer/Token32; result is a pointer-sized word',
 'android/looper.cpp': 'callback fd/events are signed I32; data is low pointer/Token32; read result with i32()',
 'android/native_activity.cpp': 'NativeActivity32 callback table, low saved-state buffers, size_t32 output, ANativeWindow/AInputQueue tokens; builder for declared types',
 'java/native_bridge.cpp': 'replace split GP/FP lists with ordered builder; JNIEnv/jobject tokens; jbyte/jchar/jshort extension; i32/i64/f32/f64 result accessors',
 'hle/aaudio/aaudio.cpp': 'stream Token32, low user/PCM buffer, I32 frame count and error; callback result i32()',
 'hle/opensles/opensles.cpp': '4-byte interface table and Token32 object; low user callback data',
 'hle/vulkan/callbacks.cpp': 'size/alignment are words; user/object tokens; allocation callbacks must return low pointers; Vulkan layout packet',
 'hle/gles/gles.cpp': 'debug callback U32 enums/id, I32 length; copy transient host message to low memory; user token',
 'hle/liblog/liblog.cpp': 'copy LogMessage32 and transient abort text to low storage; host-local message is caught by word guard',
 'hle/libz/libz.cpp': 'zalloc count/size U32, opaque token; inflateBack input uses Out<Ptr32> and low buffer; output pointer/length32',
 'hle/libc/libdl.cpp': 'dl_phdr_info32 low scratch and Size32; __cfi_check call-site type ID is a true U64 even pair',
 'hle/libc/threads.cpp': 'once callbacks take no args; cleanup/key destructor data must be low pointer/Token32',
 'hle/libc/stdlib.cpp': 'qsort/bsearch comparator pointers need low backing; context is pointer word; result i32()',
 'hle/libc/files_extra.cpp': 'nftw callback needs low pathname and stat32/FTW32 records, I32 kind; result i32()',
 'hle/libc/stdio.cpp': 'funopen cookie token and low buffer; read/write I32 lengths/results; seek Off32 vs Off64 must be separate typed builders/results',
 'hle/libc/files.cpp': 'scandir callbacks need dirent32 and low 4-byte pointer cells; h2g(&a)/h2g(&b) are host locals',
 'hle/libc/process.cpp': '__cxa_finalize null pointer word is safe; retain untyped call',
 'hle/libc/misc.cpp': 'property callbacks need low names/values/prop_info32; serial U32; destructor args low pointers/Token32',
 'hle/libandroid/choreographer.cpp': 'refresh callback F32 belongs in r0; frame time Long32 vs I64 depends on symbol; frame-data token and user pointer word',
 'hle/libandroid/services.cpp': 'storage callback filename is transient host string: copy low; I32 state and pointer-word data',
}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    rows = []
    for path in sorted((ROOT / 'src/runtime').rglob('*.cpp')):
        source = path.read_text(encoding='utf-8')
        relative = path.relative_to(ROOT / 'src/runtime').as_posix()
        for match in re.finditer(r'(?:\.|->)call\(', source):
            start = match.end(); depth = 1; end = start
            while depth and end < len(source):
                if source[end] == '(': depth += 1
                elif source[end] == ')': depth -= 1
                end += 1
            if relative not in REVIEW: raise RuntimeError('unreviewed call file: ' + relative)
            rows.append(dict(file='src/runtime/' + relative, line=source.count('\n', 0, match.start()) + 1,
                             arguments=' '.join(source[start:end-1].split()), review=REVIEW[relative]))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({'call_sites': len(rows), 'sites': rows}, indent=2) + '\n')
    print(f'{len(rows)} reviewed call sites -> {args.output}')

if __name__ == '__main__': main()
