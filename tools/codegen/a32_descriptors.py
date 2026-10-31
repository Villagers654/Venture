#!/usr/bin/env python3
"""Generate per-symbol A32 descriptors from NDK r30 clang AST JSON.

Unknown types fail closed. No host signature inference. Run with --ndk .deps/android-ndk.
Generated C++ and a machine-readable manifest are checked in separately from A64.
"""
import argparse
import json
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GROUPS = {
    'libc.so': '''assert.h ctype.h dirent.h errno.h fcntl.h fnmatch.h getopt.h glob.h grp.h iconv.h langinfo.h locale.h malloc.h netdb.h poll.h pthread.h pwd.h regex.h sched.h semaphore.h setjmp.h signal.h stdio.h stdlib.h string.h strings.h time.h uchar.h unistd.h utime.h wchar.h wctype.h arpa/inet.h net/if.h sys/auxv.h sys/epoll.h sys/eventfd.h sys/file.h sys/inotify.h sys/ioctl.h sys/mman.h sys/prctl.h sys/resource.h sys/select.h sys/sendfile.h sys/socket.h sys/stat.h sys/statvfs.h sys/syscall.h sys/sysinfo.h sys/system_properties.h sys/time.h sys/times.h sys/timerfd.h sys/types.h sys/uio.h sys/utsname.h sys/vfs.h sys/wait.h'''.split(),
    'libm.so': ['math.h', 'fenv.h', 'complex.h'],
    'libdl.so': ['dlfcn.h', 'android/dlext.h', 'link.h'],
    'liblog.so': ['android/log.h'],
    'libz.so': ['zlib.h'],
    'libandroid.so': ['android/' + s + '.h' for s in '''asset_manager asset_manager_jni bitmap choreographer configuration hardware_buffer input keycodes looper native_activity native_window native_window_jni rect sensor storage_manager obb trace window'''.split()],
    'libOpenSLES.so': ['SLES/OpenSLES.h', 'SLES/OpenSLES_Android.h'],
    'JNIEnv': ['jni.h'],
}
SEMANTIC = {'size_t': 'Size', 'ssize_t': 'SSize', 'ptrdiff_t': 'SSize', 'intptr_t': 'Long',
            'uintptr_t': 'ULong', 'off_t': 'Off', 'time_t': 'Time', 'clock_t': 'Long',
            'off64_t': 'I64', 'int64_t': 'I64', 'uint64_t': 'U64', 'intmax_t': 'I64', 'uintmax_t': 'U64'}
SCALAR = {'void': 'Void', '_Bool': 'U8', 'bool': 'U8', 'char': 'U8', 'signed char': 'I8',
          'unsigned char': 'U8', 'short': 'I16', 'unsigned short': 'U16', 'int': 'I32',
          'unsigned int': 'U32', 'long': 'Long', 'unsigned long': 'ULong',
          'long long': 'I64', 'unsigned long long': 'U64', 'float': 'F32', 'double': 'F64', 'long double': 'F64'}
OUT = {'Size': 'OutSize', 'Long': 'OutLong', 'Off': 'OutOff', 'Time': 'OutTime'}
HANDLE = {'locale_t', 'pthread_t', 'timer_t', 'jobject', 'jclass', 'jstring', 'jarray', 'jthrowable', 'jweak', 'jmethodID', 'jfieldID', 'gzFile'}

def clean(s):
    return re.sub(r'\s+', ' ', re.sub(r'\b(_Nonnull|_Nullable|_Null_unspecified|restrict|__restrict|__restrict__)\b', '', s)).strip()

def classify(spelling, aliases, depth=0):
    s = clean(spelling)
    tag = s
    if depth > 16:
        return {'kind': 'Unsupported', 'tag': tag}
    if s in SEMANTIC:
        return {'kind': SEMANTIC[s], 'tag': tag}
    if s in HANDLE or s.startswith('j') and s.endswith('Array'):
        return {'kind': 'Handle', 'tag': tag}
    if s in ('va_list', '__builtin_va_list', '__va_list'):
        return {'kind': 'VaList', 'tag': tag}
    if s in SCALAR:
        return {'kind': SCALAR[s], 'tag': tag}
    if '(*' in s or '( *' in s:
        return {'kind': 'Callback', 'tag': tag}
    if '*' in s:
        if s.count('*') > 1:
            # Pointer arrays cannot be treated as a single pointer output.
            base = classify(re.sub(r'\b(const|volatile)\b', '', s.split('*')[0]).strip(), aliases, depth + 1)
            k = 'OutPtr' if s.count('*') == 2 and 'const' not in s.split('*', 1)[1] and base['kind'] in ('Void', 'I8', 'U8', 'I16', 'U16', 'I32', 'U32', 'I64', 'U64', 'F32', 'F64') else 'Layout'
            return {'kind': k, 'tag': tag}
        pointee = clean(s.split('*')[0])
        base = classify(re.sub(r'\b(const|volatile)\b', '', pointee).strip(), aliases, depth + 1)
        if base['kind'] == 'Handle': return {'kind': 'Handle', 'tag': tag}
        if base['kind'] in OUT:
            return {'kind': 'Layout' if 'const' in pointee else OUT[base['kind']], 'tag': tag}
        if base['kind'] in ('Void', 'I8', 'U8', 'I16', 'U16', 'I32', 'U32', 'I64', 'U64', 'F32', 'F64'):
            size = {'Void': 0, 'I8': 1, 'U8': 1, 'I16': 2, 'U16': 2, 'I64': 8, 'U64': 8, 'F64': 8}.get(base['kind'], 4)
            return {'kind': 'Ptr', 'tag': tag, 'size': size}
        return {'kind': 'Layout', 'tag': tag}
    if s in aliases:
        if aliases[s] == '@opaque': return {'kind': 'Handle', 'tag': tag}
        result = classify(aliases[s], aliases, depth + 1)
        return dict(result, tag=tag)
    if s.startswith('const '):
        return classify(s[6:], aliases, depth + 1)
    if s.startswith('enum '):
        return {'kind': 'I32', 'tag': tag}
    return {'kind': 'Layout' if s.startswith(('struct ', 'union ')) else 'Unsupported', 'tag': tag}

def nodes(node):
    yield node
    for child in node.get('inner', []):
        yield from nodes(child)

def parse(ast, library, headers):
    aliases = {n['name']: n['type']['qualType'] for n in nodes(ast)
               if n.get('kind') == 'TypedefDecl' and 'type' in n and 'name' in n}
    records = [n for n in nodes(ast) if n.get('kind') == 'RecordDecl' and n.get('name')]
    complete = {n['name'] for n in records if n.get('completeDefinition')}
    for n in records:
        if n['name'] not in complete: aliases['struct ' + n['name']] = '@opaque'
    rows = {}
    current_file = ''
    for n in ast.get('inner', []):
        loc = n.get('loc', {})
        current_file = loc.get('file', loc.get('spellingLoc', {}).get('file', current_file))
        if n.get('kind') != 'FunctionDecl' or n.get('isImplicit'):
            continue
        # libc owns dependencies (fortify, bits/, etc); other libraries own
        # declarations from exactly their public headers.
        if library != 'libc.so' and not any(current_file.replace('\\', '/').endswith('/' + h) for h in headers):
            continue
        name = n['name']
        signature = n['type']['qualType']
        result = signature.split('(', 1)[0].strip()
        params = [x for x in n.get('inner', []) if x.get('kind') == 'ParmVarDecl']
        rows[name] = dict(library=library, symbol=name, result=classify(result, aliases),
                          arguments=[classify(x['type']['qualType'], aliases) for x in params],
                          variadic=n.get('variadic', False), source='NDK r30 ' + Path(current_file).name)
        if library == 'libc.so' and name == 'strerror_r':
            # string.h: _GNU_SOURCE declares a char* function renamed in ELF
            # to __gnu_strerror_r. The actual strerror_r symbol is the int
            # XSI entry point. Descriptors are keyed by ELF symbol names.
            rows[name]['result'] = classify('int', {})
            rows[name]['source'] = 'NDK r30 string.h XSI strerror_r ELF symbol'
        if name in ('getsockopt', 'setsockopt') and library == 'libc.so':
            # sys/socket.h: option_value is discriminator-dependent, including
            # struct timeval for SO_RCVTIMEO/SO_SNDTIMEO. A void* is not proof
            # of a byte-compatible payload; require the socket-option adapter.
            rows[name]['arguments'][3] = {'kind': 'Layout', 'tag': 'socket option payload'}
        if library == 'libc.so' and name in ('setjmp', '_setjmp', 'sigsetjmp', 'longjmp', '_longjmp', 'siglongjmp'):
            # Clang adjusts the jmp_buf array parameter to long*. It is a
            # 256/260-byte record, never a single long output cell.
            rows[name]['arguments'][0] = {'kind': 'Layout', 'tag': 'ARM jmp_buf'}
    # JNI and OpenSL interface methods are record fields of function-pointer
    # type. Preserve the first env/self slot; tokens/layouts require adapters.
    for n in nodes(ast):
        if n.get('kind') != 'RecordDecl':
            continue
        if library == 'JNIEnv':
            if n.get('name') not in ('JNINativeInterface', 'JNIInvokeInterface'): continue
        elif library == 'libOpenSLES.so':
            if not n.get('name', '').startswith('SL'): continue
        else:
            continue
        record = n['name']
        for field in n.get('inner', []):
            if field.get('kind') != 'FieldDecl':
                continue
            sig = field['type']['qualType']
            m = re.match(r'(.+?)\s*\(\*\)\((.*)\)$', sig)
            if not m:
                continue
            args = [a.strip() for a in m[2].split(',') if a.strip() != 'void']
            variadic = args and args[-1] == '...'
            if variadic: args.pop()
            symbol = field['name']
            lib = ('JavaVM' if record == 'JNIInvokeInterface' else 'JNIEnv') if library == 'JNIEnv' else library
            # These are the existing runtime's interface-slot registration
            # names, rather than ELF exports. Android queue uses BufferQueue's
            # table; its token/layout adapter remains explicit.
            key = symbol if library == 'JNIEnv' else record.removeprefix('SL').removesuffix('Itf_') + '.' + symbol
            rows[key] = dict(library=lib, symbol=key, result=classify(m[1], aliases), arguments=[classify(a, aliases) for a in args], variadic=bool(variadic), source='NDK r30 ' + record)
    return list(rows.values())

def type_cpp(t):
    return '{Kind::' + t['kind'] + ', ' + json.dumps(t.get('tag', '')) + ', ' + str(t.get('size', 0)) + ', ' + str(t.get('alignment', 4)) + '}'

def emit(rows, path, function):
    rows = sorted(rows, key=lambda r: (r['library'], r['symbol']))
    assert len({(r['library'], r['symbol']) for r in rows}) == len(rows), 'duplicate descriptor key'
    lines = ['// Generated by tools/codegen/a32_descriptors.py / gl_thunks.py. Do not edit.', '#include "core/a32_types.h"', 'namespace venture::hle::a32 {', 'namespace {']
    for i, row in enumerate(rows):
        if row['arguments']:
            lines.append(f'constexpr Type args_{i}[] = {{' + ', '.join(map(type_cpp, row['arguments'])) + '};')
    lines.append('const Descriptor descriptors[] = {')
    for i, row in enumerate(rows):
        lines.append('  {' + ', '.join([json.dumps(row['library']), json.dumps(row['symbol']), type_cpp(row['result']), f'args_{i}' if row['arguments'] else '{}', str(row.get('variadic', False)).lower(), json.dumps(row['source'])]) + '},')
    lines += ['};', '}', f'std::span<const Descriptor> {function}() {{ return descriptors; }}', '}']
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    path.with_suffix('.json').write_text('[\n' + ',\n'.join(json.dumps(r) for r in rows) + '\n]\n', encoding='utf-8')
    print(f'{path.name}: {len(rows)} descriptors')

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--ndk', type=Path, default=ROOT / '.deps/android-ndk')
    p.add_argument('--output', type=Path, default=ROOT / 'src/runtime/core/generated')
    p.add_argument('--libc-supplement', action='store_true', help='emit API 32 libc/libm/libdl declarations absent from the API 23 base')
    a = p.parse_args()
    prebuilt = next((a.ndk / 'toolchains/llvm/prebuilt').iterdir())
    clang = prebuilt / 'bin' / ('clang.exe' if (prebuilt / 'bin/clang.exe').exists() else 'clang')
    if not clang.exists(): clang = prebuilt / 'bin/clang'
    rows = []
    with tempfile.TemporaryDirectory(prefix='venture-a32-ast-') as tmp:
        for library, headers in GROUPS.items():
            if a.libc_supplement and library not in ('libc.so', 'libm.so', 'libdl.so'): continue
            source = Path(tmp) / 'probe.c'
            source.write_text('#define _GNU_SOURCE 1\n' + ''.join(f'#include <{h}>\n' for h in headers))
            command = [str(clang), '--target=armv7a-linux-androideabi' + ('32' if a.libc_supplement else '23'), '-x', 'c', '-fsyntax-only', '-Wno-deprecated-declarations', '-Xclang', '-ast-dump=json', str(source)]
            run = subprocess.run(command, capture_output=True, text=True, encoding='utf-8')
            if run.returncode:
                raise RuntimeError(run.stderr)
            rows += parse(json.loads(run.stdout), library, headers)
    if a.libc_supplement:
        base = json.loads((a.output / 'a32_ndk.json').read_text())
        existing = {(r['library'], r['symbol']) for r in base}
        by_name = {(r['library'], r['symbol']): r for r in base + rows}
        # Bionic ABI aliases/private entry points not declared in the public
        # non-fortified headers: libc.map.txt, bits/fortify/stdio.h,
        # bionic/pthread_atfork.cpp and arch-arm/bionic/setjmp.S.
        for alias, original in [('fcntl64', 'fcntl'), ('bsd_signal', 'signal'), ('__sigsetjmp', 'sigsetjmp')]:
            rows.append(dict(by_name['libc.so', original], symbol=alias, source='bionic ABI alias of ' + original))
        for name in ('fread', 'fwrite'):
            row = by_name['libc.so', name]
            rows.append(dict(row, symbol='__' + name + '_chk', arguments=row['arguments'] + [classify('size_t', {})], source='NDK r30 bits/fortify/stdio.h'))
        row = by_name['libc.so', 'pthread_atfork']
        rows.append(dict(row, symbol='__register_atfork', arguments=row['arguments'] + [classify('void *', {})], source='bionic/pthread_atfork.cpp'))
        rows[-1]['result'] = {'kind': 'Void'}
        # bionic libc/bionic/__cxa_atexit.cpp, __cxa_thread_atexit_impl.cpp.
        for name in ('__cxa_atexit', '__cxa_thread_atexit', '__cxa_thread_atexit_impl'):
            rows.append(dict(library='libc.so', symbol=name, result=classify('int', {}), arguments=[classify(t, {}) for t in ('void (*)(void *)', 'void *', 'void *')], source='bionic C++ ABI destructor entry points'))
        # ARM C++ ABI 3.3.5.3: __aeabi_atexit(object, destroyer, dso) is
        # __cxa_atexit with the first two arguments swapped (bionic arch-arm).
        rows.append(dict(library='libc.so', symbol='__aeabi_atexit', result=classify('int', {}), arguments=[classify(t, {}) for t in ('void *', 'void (*)(void *)', 'void *')], source='ARM C++ ABI 3.3.5.3 / bionic arch-arm atexit'))
        rows.append(dict(library='libc.so', symbol='__cxa_finalize', result=classify('void', {}), arguments=[classify('void *', {})], source='bionic __cxa_atexit.cpp'))
        rows.append(dict(by_name['libc.so', 'strerror_r'], symbol='__gnu_strerror_r', result=classify('char *', {}), source='NDK r30 string.h __RENAME(__gnu_strerror_r)'))
        rows.append(dict(library='libc.so', symbol='__stack_chk_fail', result=classify('void', {}), arguments=[], source='bionic __stack_chk_fail.cpp'))
        # The linker's crash handler is reachable only as the sa_sigaction that
        # debuggerd_init() installs; guest crash reporters call it when chaining.
        rows.append(dict(library='libc.so', symbol='<debuggerd_signal_handler>', result=classify('void', {}), arguments=[classify(t, {}) for t in ('int', 'siginfo_t *', 'void *')], source='system/core/debuggerd/handler/debuggerd_handler.cpp sa_sigaction'))
        rows.append(dict(library='libc.so', symbol='<condition_wait_personality32>', result=classify('unsigned int', {}), arguments=[classify(t, {}) for t in ('unsigned int', 'void *', 'void *')], source='Arm EHABI32 personality routine ABI'))
        for alias, original in [('__isfinitef', 'finitef'), ('__isfinite', 'finite')]:
            rows.append(dict(by_name['libm.so', original], symbol=alias, source='bionic libm.map.txt alias of ' + original))
        rows.append(dict(library='libdl.so', symbol='android_get_exported_namespace',
                         result={'kind': 'Ptr', 'tag': 'android_namespace_t *'},
                         arguments=[classify('const char *', {})],
                         source='bionic linker exported-namespace API (private dlfcn extension)'))
        rows = [r for r in rows if (r['library'], r['symbol']) not in existing]
        emit(rows, a.output / 'a32_libc.cpp', 'libc_descriptors')
        return
    # bionic libc/arch-arm/bionic/exidx_dynamic.c forwards this compiler-runtime
    # name with exactly the public link.h signature; derive, do not infer from F.
    # https://android.googlesource.com/platform/bionic/+/ics-mr1-release/libc/arch-arm/bionic/exidx_dynamic.c
    exidx = next(r for r in rows if (r['library'], r['symbol']) == ('libdl.so', 'dl_unwind_find_exidx'))
    rows.append(dict(exidx, symbol='__gnu_Unwind_Find_exidx', source='NDK r30 link.h / bionic exidx_dynamic.c forwarding ABI'))
    # ARM Run-time ABI section 4.3.4: compiler-rt builtins are not declared by
    # libc headers; their declarations live in compiler-rt, not host signatures.
    for base, args in [('memcpy', ['void *', 'const void *', 'size_t']), ('memmove', ['void *', 'const void *', 'size_t']), ('memset', ['void *', 'size_t', 'int']), ('memclr', ['void *', 'size_t'])]:
        for suffix in ('', '4', '8'):
            rows.append(dict(library='libc.so', symbol='__aeabi_' + base + suffix, result={'kind': 'Void'}, arguments=[classify(s, {}) for s in args], source='ARM RTABI32 4.3.4 / compiler-rt builtins'))
    # Guest-callable default allocator thunks use zlib.h's alloc_func/free_func
    # typedefs. They are internal symbols, with the same ABI as guest callbacks.
    for name, result, args in [('<A32 zalloc>', 'void *', ['void *', 'unsigned int', 'unsigned int']),
                               ('<A32 zfree>', 'void', ['void *', 'void *'])]:
        rows.append(dict(library='libz.so', symbol=name, result=classify(result, {}),
                         arguments=[classify(t, {}) for t in args], source='NDK r30 zlib.h alloc_func/free_func'))
    emit(rows, a.output / 'a32_ndk.cpp', 'ndk_descriptors')

if __name__ == '__main__': main()
