"""Extract JVMS 4.1/4.4 type ancestry only; no SDK bytecode enters the runtime."""
import struct


def surface(data):
    """Read ancestry and exact field/method descriptors, without loading classes."""
    pos = 8
    def u2():
        nonlocal pos
        value = struct.unpack_from('>H', data, pos)[0]
        pos += 2
        return value
    if data[:4] != b'\xca\xfe\xba\xbe':
        raise ValueError('Invalid class header')
    pool = [None] * u2()
    index = 1
    while index < len(pool):
        tag = data[pos]
        pos += 1
        if tag == 1:
            size = u2()
            pool[index] = data[pos:pos + size]
            pos += size
        elif tag == 7:
            pool[index] = u2()
        elif tag in (3, 4, 9, 10, 11, 12, 17, 18):
            pos += 4
        elif tag in (5, 6):
            pos += 8
            index += 1
        elif tag in (8, 16, 19, 20):
            pos += 2
        elif tag == 15:
            pos += 3
        else:
            raise ValueError(f'Unknown constant-pool tag {tag}')
        index += 1
    def name(index):
        # SDK type names are ASCII; string/method constants are never decoded.
        return pool[pool[index]].decode('ascii') if index else ''
    flags, owner, parent = u2(), u2(), u2()
    interfaces = [name(u2()) for _ in range(u2())]
    result = dict(name=name(owner), parents=([name(parent)] if parent else []) + interfaces,
                  parent=name(parent), interfaces=interfaces, flags=flags)
    for kind in ('fields', 'methods'):
        members = {}
        for _ in range(u2()):
            access, member, descriptor = u2(), u2(), u2()
            members[(pool[member].decode('utf-8', 'surrogatepass'),
                     pool[descriptor].decode('utf-8', 'surrogatepass'))] = access
            for _ in range(u2()):
                u2()
                size = struct.unpack_from('>I', data, pos)[0]
                pos += 4 + size
        result[kind] = members
    return result


def declaration(data):
    result = surface(data)
    return '\t'.join([result['name'], result['parent'], str(int(bool(result['flags'] & 0x200))),
                      ','.join(result['interfaces'])])
