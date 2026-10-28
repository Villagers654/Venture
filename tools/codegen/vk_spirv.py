"""Embed a generated SPIR-V module, independent of compiler byte-array syntax."""
import pathlib
import struct
import sys
data = pathlib.Path(sys.argv[1]).read_bytes()
words = struct.unpack('<' + 'I' * (len(data)//4), data)
assert words[0] == 0x07230203
pathlib.Path(sys.argv[2]).write_text('static constexpr std::uint32_t decode_shader[] = {\n' +
    '\n'.join('  ' + ', '.join(hex(w) for w in words[i:i+8]) + ',' for i in range(0, len(words), 8)) + '\n};\n')
