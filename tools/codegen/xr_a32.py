"""ILP32 (A32 guest) OpenXR wire layouts and command adapters, from the registry.

Called by xr_generate.py. OpenXR handles, atoms and XrTime are 64-bit on every
ABI and AAPCS32 aligns 64-bit scalars to 8 like LP64, so an ILP32 structure
differs from the host LP64 one only by its pointer members and the padding they
move. Each structure is laid out twice by the same rules (pointer size 4 and 8);
the generated C++ asserts the LP64 result against the host compiler for every
structure the host headers declare, so the ILP32 table rests on a checked model.

Member kinds (hle/openxr/a32.cpp converts them):
  Data       bytes with the same encoding on both sides (scalars, arrays,
             pointer-free nested structures)
  Nested     a by-value structure (array) that itself holds pointers
  Next       the structure chain
  Opaque     a pointer whose pointee needs no conversion (strings, scalar and
             pointer-free structure buffers, EGL/JNI identities); widened
  StructPtr  pointer to a structure (array, `len` member) that holds pointers
  PtrArray   pointer to an array of pointers (`len` member)
Structures with callbacks or non-Android platform types are left out; a guest
chain node of such a type is dropped as an unknown structure.
"""
import re

SCALARS = {'uint8_t': 1, 'int8_t': 1, 'char': 1, 'uint16_t': 2, 'int16_t': 2,
           'uint32_t': 4, 'int32_t': 4, 'float': 4, 'uint64_t': 8, 'int64_t': 8,
           'double': 8, 'EGLenum': 4}
# Pointer-sized identities passed by value (EGL/JNI handles of the guest).
OPAQUE_VALUE = {'EGLDisplay', 'EGLConfig', 'EGLContext', 'jobject'}
# Pointees forwarded as identities.
OPAQUE_POINTEE = {'void', 'char', 'ANativeWindow', 'AIBinder'}
# Platform commands the HLE implements outside the generated core set.
EXTRA_COMMANDS = ['xrSetAndroidApplicationThreadKHR', 'xrGetOpenGLESGraphicsRequirementsKHR',
                  'xrConvertTimespecTimeToTimeKHR', 'xrConvertTimeToTimespecTimeKHR']
LOADER_ALIASES = {'_Z21xrInitializeLoaderKHRPK29XrLoaderInitInfoBaseHeaderKHR': 'xrInitializeLoaderKHR'}
KINDS = ['Data', 'Nested', 'Next', 'Opaque', 'StructPtr', 'PtrArray']


def text(e): return ''.join(e.itertext()).strip()


def align_up(v, a): return (v + a - 1) // a * a


class Model:
    def __init__(self, xml, types):
        self.types = types
        self.constants = {e.get('name'): int(e.get('value')) for e in xml.findall("enums[@name='API Constants']/enum")
                          if e.get('value', '').isdigit()}
        # Array sizes some extensions define (XR_HAND_TRACKING_CAPSULE_POINT_COUNT_FB, ...).
        for e in xml.findall('extensions/extension/require/enum'):
            if not e.get('extends') and e.get('value', '').isdigit():
                self.constants.setdefault(e.get('name'), int(e.get('value')))
        self.values = {}
        for e in xml.findall("enums[@name='XrStructureType']/enum"):
            if e.get('value') is not None: self.values[e.get('name')] = int(e.get('value'))
        for parent, number in [(f, None) for f in xml.findall('feature')] + \
                [(x, int(x.get('number'))) for x in xml.findall('extensions/extension')]:
            for e in parent.findall("require/enum[@extends='XrStructureType']"):
                if e.get('alias'): continue
                if e.get('value') is not None: value = int(e.get('value'))
                else:
                    ext = int(e.get('extnumber', number or 0))
                    value = 1000000000 + (ext - 1) * 1000 + int(e.get('offset'))
                    if e.get('dir') == '-': value = -value
                self.values[e.get('name')] = value
        # Structures the host openxr.h declares: required by a supported
        # feature/extension without platform protection or beta gating.
        self.host = set()
        self.owner = {}
        for parent in xml.findall('feature') + xml.findall('extensions/extension'):
            plain = parent.tag == 'feature' or (parent.get('supported') == 'openxr' and not parent.get('protect')
                                                and parent.get('provisional') != 'true')
            for t in parent.findall('require/type'):
                name = t.get('name')
                self.owner.setdefault(name, parent)
                if plain: self.host.add(name)
        self.memo = {}

    def base(self, t):
        if t in SCALARS: return ('scalar', SCALARS[t])
        if t in OPAQUE_VALUE: return ('opaque',)
        node = self.types.get(t)
        if node is None: return None
        if node.get('alias'): return self.base(node.get('alias'))
        cat = node.get('category')
        if cat == 'enum': return ('scalar', 4)
        if cat == 'handle': return ('scalar', 8)
        if cat in ('bitmask', 'basetype'):
            inner = node.findtext('type')
            if inner in ('XR_DEFINE_ATOM', 'XR_DEFINE_OPAQUE_64'): return ('scalar', 8)
            return self.base(inner) if inner else None
        if cat == 'struct': return ('struct', t)
        return None

    def struct(self, name):
        node = self.types.get(name)
        if node is not None and node.get('alias'): return self.struct(node.get('alias'))
        if name in self.memo: return self.memo[name]
        self.memo[name] = None  # cycles through by-value members are unsupported
        if node is None or node.get('category') != 'struct': return None
        members = []
        siblings = [m.findtext('name') for m in node.findall('member')]
        for m in node.findall('member'):
            mtext, typ, mname = text(m), m.findtext('type'), m.findtext('name')
            head, _, tail = mtext.rpartition(mname)
            stars = head.count('*')
            count = 1
            for dim in re.findall(r'\[([^\]]+)\]', tail):
                dim = dim.strip()
                count *= int(dim) if dim.isdigit() else self.constants.get(dim, 0)
            if count == 0: return None
            const = head.strip().startswith('const')
            b = self.base(typ)
            length = m.get('len', '').split(',')[0]
            index = siblings.index(length) if length in siblings else -1
            row = dict(name=mname, const=const, target=None, len=index, count=count)
            if stars == 0:
                if b is None: return None
                if b[0] == 'scalar': row.update(kind='Data', size=b[1] * count, align=b[1])
                elif b[0] == 'opaque':
                    if count != 1: return None
                    row.update(kind='Opaque')
                else:
                    s = self.struct(b[1])
                    if s is None: return None
                    if s['flat']: row.update(kind='Data', size=s['size'][4] * count, align=s['align'][4])
                    else: row.update(kind='Nested', target=b[1])
            elif stars == 1:
                if count != 1: return None
                if mname == 'next': row.update(kind='Next')
                elif typ in OPAQUE_POINTEE or (b is not None and b[0] == 'scalar'): row.update(kind='Opaque')
                elif b is None or b[0] != 'struct': return None
                else:
                    s = self.struct(b[1])
                    if s is None: return None
                    row.update(kind='Opaque') if s['flat'] else row.update(kind='StructPtr', target=b[1])
            elif stars == 2:
                if count != 1 or index < 0: return None
                if typ in OPAQUE_POINTEE or (b is not None and b[0] == 'scalar'): row.update(kind='PtrArray')
                elif b is None or b[0] != 'struct': return None
                else:
                    s = self.struct(b[1])
                    if s is None: return None
                    row.update(kind='PtrArray', target=None if s['flat'] else b[1])
            else:
                return None
            members.append(row)
        result = dict(name=name, members=members, size={}, align={}, offsets={})
        for ptr in (4, 8):
            offset, top, offsets = 0, 1, []
            for row in members:
                if row['kind'] == 'Data': size, align = row['size'], row['align']
                elif row['kind'] == 'Nested':
                    s = self.memo[self.resolve(row['target'])]
                    size, align = s['size'][ptr] * row['count'], s['align'][ptr]
                else: size, align = ptr, ptr
                offset = align_up(offset, align)
                offsets.append(offset)
                offset += size
                top = max(top, align)
            result['size'][ptr], result['align'][ptr], result['offsets'][ptr] = align_up(offset, top), top, offsets
        result['flat'] = all(row['kind'] == 'Data' for row in members)
        stype = node.find("member[name='type']")
        result['type'] = self.values.get(stype.get('values')) if stype is not None and stype.get('values') else None
        result['host'] = name in self.host and not any(self.types.get(m.findtext('type')) is not None and
                                                       self.types[m.findtext('type')].get('requires')
                                                       for m in node.findall('member'))
        self.memo[name] = result
        return result

    def resolve(self, name):
        node = self.types.get(name)
        return self.resolve(node.get('alias')) if node is not None and node.get('alias') else name


def param(model, p, params):
    """One command parameter: (kind, descriptor kind, extra)."""
    ptext, typ, pname = text(p), p.findtext('type'), p.findtext('name')
    head = ptext.rpartition(pname)[0]
    stars, const = head.count('*'), head.strip().startswith('const')
    b = model.base(typ)
    if stars == 0:
        if b is None or b[0] != 'scalar': return None
        if typ == 'float': return ('f32', 'F32', None)
        if b[1] == 8: return ('u64', 'I64' if typ in ('int64_t', 'XrTime', 'XrDuration') else 'U64', None)
        node = model.types.get(typ)
        signed = typ == 'int32_t' or (node is not None and node.get('category') == 'enum')
        return ('word', 'I32' if signed else 'U32', None)
    if stars != 1: return None
    if typ == 'PFN_xrVoidFunction': return ('pfn', 'Ptr', None)
    if typ == 'timespec': return ('timespec_in' if const else 'timespec_out', 'Layout', None)
    if typ in ('void', 'char') or (b is not None and b[0] == 'scalar'): return ('ptr', 'Ptr', None)
    if b is None or b[0] != 'struct': return None
    s = model.struct(b[1])
    if s is None: return None
    if s['flat']: return ('ptr', 'Ptr', None)
    length = p.get('len', '')
    names = [q.findtext('name') for q in params]
    count = names.index(length) if length in names else -1
    return ('in' if const else 'inout', 'Layout', (model.resolve(b[1]), count))


def generate(xml, types, commands, selected, emit, output, core):
    model = Model(xml, types)
    # Every supported structure of the registry, so any extension's chain node converts.
    names = sorted(n for n, t in types.items() if t.get('category') == 'struct' and not t.get('alias'))
    structs = [model.struct(n) for n in names]
    structs = [s for s in structs if s is not None]
    index = {s['name']: i for i, s in enumerate(structs)}
    rows, table, checks = [], [], []
    for s in structs:
        first = len(rows)
        for row, a, h in zip(s['members'], s['offsets'][4], s['offsets'][8]):
            target = index[model.resolve(row['target'])] if row['target'] else -1
            size = row['size'] if row['kind'] == 'Data' else row['count']
            rows.append(f'  {{Kind32::{row["kind"]}, {int(row["const"])}, {a}, {h}, {size}, {target}, {row["len"]}}},  // {s["name"]}::{row["name"]}')
        stype = s['type'] if s['type'] is not None else 0
        table.append(f'  {{"{s["name"]}", static_cast<XrStructureType>({stype}), {s["size"][4]}, {s["size"][8]}, '
                     f'{s["align"][8]}, {first}, {len(s["members"])}}},')
        if s['host']:
            terms = [f'sizeof({s["name"]}) == {s["size"][8]}', f'alignof({s["name"]}) == {s["align"][8]}']
            terms += [f'offsetof({s["name"]}, {row["name"]}) == {h}' for row, h in zip(s['members'], s['offsets'][8])]
            checks.append(f'static_assert({" && ".join(terms)});')
    lines = ['#ifdef VENTURE_XR32_LAYOUTS',
             f'constexpr std::size_t kStructCount = {len(structs)};',
             'constexpr Member32 kMembers[]{', *rows, '};',
             'constexpr Struct32 kStructs[]{', *table, '};',
             '// The ILP32 model laid out with 8-byte pointers is the host compiler\'s LP64 layout.',
             *checks, '#endif']
    # Commands.
    adapters, registrations, descriptors, unsupported = [], [], [], []
    for name in sorted(set(selected) | set(EXTRA_COMMANDS)):
        cmd = commands[name]
        if cmd.get('alias'): cmd = commands[cmd.get('alias')]
        params = cmd.findall('param')
        kinds = [param(model, p, params) for p in params]
        if any(k is None for k in kinds):
            unsupported.append(name)
            continue
        reads, args = [], []
        for i, (kind, _, extra) in enumerate(kinds):
            reads.append(f'const auto a{i} = c.{"u64" if kind == "u64" else "word"}();')
            if kind in ('u64', 'word'): args.append(f'Value{{a{i}}}')
            elif kind == 'f32': args.append(f'Float{{a{i}}}')
            elif kind == 'ptr': args.append(f'c.identity(a{i})')
            elif kind == 'pfn': args.append(f'c.pfn_out(a{i})')
            elif kind in ('timespec_in', 'timespec_out'): args.append(f'c.{kind}(a{i})')
            else:
                target, count = extra
                n = f'a{count}' if count >= 0 else '1u'
                args.append(f'c.{kind}(a{i}, {index[target]}, {n})')
        adapters.append(f'void xr32_{name}(Call32& c) {{')
        adapters.append('  ' + ' '.join(reads))
        adapters.append(f'  c.finish(guest::{name}({", ".join(args)}));')
        adapters.append('}')
        registrations.append(f'VENTURE_XR32_COMMAND({name}, {name})')
        types_ = ', '.join(f'{{Kind::{k[1]}, "{p.findtext("type")}", 0, {8 if k[0] == "u64" else 4}}}'
                           for k, p in zip(kinds, params))
        descriptors.append((name, types_))
        for alias, target in LOADER_ALIASES.items():
            if target == name:
                registrations.append(f'VENTURE_XR32_COMMAND({alias}, {name})')
                descriptors.append((alias, types_))
    lines += ['#ifdef VENTURE_XR32_ADAPTERS', *adapters, '#endif',
              '#ifdef VENTURE_XR32_COMMANDS', *registrations, '#endif',
              '// Not adapted (Vulkan or non-Android platform types): ' + ' '.join(unsupported)]
    emit(output / 'a32.inc', lines)
    body = ['#include "core/a32_types.h"', 'namespace venture::hle::a32 {', 'namespace {']
    for i, (name, types_) in enumerate(sorted(descriptors)):
        body.append(f'constexpr Type xr_args_{i}[] = {{{types_}}};')
    body.append('constexpr Descriptor descriptors[] = {')
    for i, (name, _) in enumerate(sorted(descriptors)):
        body.append(f'  {{"libopenxr_loader.so", "{name}", {{Kind::I32, "XrResult", 0, 4}}, xr_args_{i}, false, "OpenXR registry"}},')
    body += ['};', '}', 'std::span<const Descriptor> openxr_descriptors() { return descriptors; }', '}']
    emit(core / 'a32_openxr.cpp', body)
    return unsupported
