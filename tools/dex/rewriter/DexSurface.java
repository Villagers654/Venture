package com.venture.dex;

import java.security.MessageDigest;
import java.util.*;
import java.util.regex.Pattern;
import java.util.zip.*;

/** Strict DEX declaration census, ported from the r2-java research parser.
 *
 * Format: https://source.android.com/docs/core/runtime/dex-format
 * No code is executed. DEX 041 containers and compact DEX require another reader.
 * Same checks and results as tools/dex/dex_surface.py (Dex, read_apk), which
 * remains for build-time census tools.
 */
public final class DexSurface {
    private final byte[] data;
    private final String name;
    final boolean checksumValid;
    final Set<String> classes = new HashSet<>();
    /** [class, name, proto, static] per native method. */
    final Set<List<Object>> natives = new HashSet<>();

    DexSurface(byte[] data, String name) {
        this.data = data;
        this.name = name;
        boolean magic = false;
        for (String v : new String[]{"035", "037", "038", "039", "040"})
            magic |= Arrays.equals(Arrays.copyOf(data, Math.min(8, data.length)), ("dex\n" + v + "\0").getBytes(java.nio.charset.StandardCharsets.ISO_8859_1));
        if (!magic) throw new IllegalArgumentException(name + ": unsupported DEX magic " + bytesRepr(Arrays.copyOf(data, Math.min(8, data.length))));
        if (data.length < 112 || u32(32) != data.length || u32(36) != 112 || u32(40) != 0x12345678L)
            throw new IllegalArgumentException(name + ": invalid DEX header");
        // ART validates Adler-32, not the SHA-1 identification field. The owned
        // RE4 APK has a stale SHA-1 but a valid Adler-32 (including that field).
        // AOSP android12L-release, libdexfile/dex/dex_file_verifier.cc: CheckHeader
        Adler32 adler = new Adler32();
        adler.update(data, 12, data.length - 12);
        checksumValid = adler.getValue() == u32(8);
        if (!checksumValid) throw new IllegalArgumentException(name + ": DEX checksum mismatch");
        String[] strings = new String[count(56)];
        for (int i = 0; i < strings.length; ++i) strings[i] = string(u32(u32(60) + 4L * i));
        String[] types = new String[count(64)];
        for (int i = 0; i < types.length; ++i) types[i] = strings[index(u32(u32(68) + 4L * i), strings.length)];
        String[] protos = new String[count(72)];
        for (int i = 0; i < protos.length; ++i) {
            long at = u32(76) + 12L * i;
            u32(at);  // shorty_idx, unused
            long ret = u32(at + 4), params = u32(at + 8);
            protos[i] = "(" + String.join("", typelist(params, types)) + ")" + types[index(ret, types.length)];
        }
        String[][] methods = new String[count(88)][];
        for (int i = 0; i < methods.length; ++i) {
            long at = u32(92) + 8L * i;
            int cls = u16(at), proto = u16(at + 2);
            long nm = u32(at + 4);
            methods[i] = new String[]{strip(types[index(cls, types.length)]), strings[index(nm, strings.length)], protos[index(proto, protos.length)]};
        }
        String[][] fields = new String[count(80)][];
        for (int i = 0; i < fields.length; ++i) {
            long at = u32(84) + 8L * i;
            int cls = u16(at), type = u16(at + 2);
            long nm = u32(at + 4);
            fields[i] = new String[]{strip(types[index(cls, types.length)]), strings[index(nm, strings.length)], types[index(type, types.length)]};
        }
        int classDefs = count(96);
        for (int i = 0; i < classDefs; ++i) {
            long at = u32(100) + 32L * i;
            long cls = u32(at), parent = u32(at + 8), interfaces = u32(at + 12);
            long p = u32(at + 24);
            for (long off = at; off < at + 32; off += 4) u32(off);
            String owner = strip(types[index(cls, types.length)]);
            if (!classes.add(owner)) throw new IllegalArgumentException(name + ": duplicate class " + owner);
            // Ancestry is not part of the result, but invalid references fail as in the Python parser.
            if (parent != 0xffffffffL) strip(types[index(parent, types.length)]);
            typelist(interfaces, types);
            if (p == 0) continue;
            long[] sizes = new long[4];
            long[] cursor = {p};
            for (int k = 0; k < 4; ++k) sizes[k] = uleb(cursor);
            for (int list = 0; list < 2; ++list) {
                long idx = 0;
                for (long n = 0; n < sizes[list]; ++n) {
                    idx += uleb(cursor);
                    uleb(cursor);
                    String[] unused = fields[index(idx, fields.length)];
                }
            }
            for (int list = 2; list < 4; ++list) {
                long idx = 0;
                for (long n = 0; n < sizes[list]; ++n) {
                    idx += uleb(cursor);
                    long flags = uleb(cursor), code = uleb(cursor);
                    String[] method = methods[index(idx, methods.length)];
                    if (!method[0].equals(owner)) throw new IllegalArgumentException(name + ": method owner mismatch");
                    if ((flags & 0x100) != 0) {
                        if (code != 0) throw new IllegalArgumentException(name + ": native method contains code");
                        natives.add(List.of(method[0], method[1], method[2], (flags & 8) != 0));
                    }
                }
            }
        }
    }

    /** Same DEX identity as readApk, with bounded memory and no DEX parsing. */
    static String identityApk(ZipFile archive, List<String> names) throws Exception {
        Pattern root = Pattern.compile("classes(?:[0-9]+)?\\.dex");
        for (var entries = archive.entries(); entries.hasMoreElements(); ) {
            String n = entries.nextElement().getName();
            if (root.matcher(n).matches()) names.add(n);
        }
        if (names.isEmpty()) throw new IllegalArgumentException("APK contains no root classes*.dex");
        names.sort(Comparator.naturalOrder());
        MessageDigest combined = MessageDigest.getInstance("SHA-256");
        byte[] single = null;
        byte[] buffer = new byte[65536];
        for (String name : names) {
            ZipEntry entry = archive.getEntry(name);
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            CRC32 crc = new CRC32();
            try (var in = archive.getInputStream(entry)) {
                for (int n; (n = in.read(buffer)) != -1;) {
                    digest.update(buffer, 0, n);
                    crc.update(buffer, 0, n);
                }
            }
            if (entry.getCrc() != -1 && crc.getValue() != entry.getCrc())
                throw new ZipException("Bad CRC-32 for file '" + name + "'");
            single = digest.digest();
            combined.update(name.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            combined.update((byte)0);
            combined.update(single);
        }
        return hex(names.size() == 1 ? single : combined.digest());
    }

    /** Reads root classes*.dex in name order into `names`/`dexes`, adds the census of
     * all of them to `classes`/`natives`, and returns the DEX identity hash. */
    static String readApk(ZipFile archive, List<String> names, List<byte[]> dexes,
                          Set<String> classes, Set<List<Object>> natives) throws Exception {
        Pattern root = Pattern.compile("classes(?:[0-9]+)?\\.dex");
        for (var entries = archive.entries(); entries.hasMoreElements(); ) {
            String n = entries.nextElement().getName();
            if (root.matcher(n).matches()) names.add(n);
        }
        if (names.isEmpty()) throw new IllegalArgumentException("APK contains no root classes*.dex");
        names.sort(Comparator.naturalOrder());
        for (String n : names) {
            ZipEntry entry = archive.getEntry(n);
            byte[] bytes;
            try (var in = archive.getInputStream(entry)) { bytes = in.readAllBytes(); }
            CRC32 crc = new CRC32();
            crc.update(bytes);
            if (entry.getCrc() != -1 && crc.getValue() != entry.getCrc())
                throw new ZipException("Bad CRC-32 for file '" + n + "'");
            dexes.add(bytes);
        }
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        for (int i = 0; i < names.size(); ++i) {
            DexSurface dex = new DexSurface(dexes.get(i), names.get(i));
            for (String c : dex.classes)
                if (classes.contains(c)) throw new IllegalArgumentException("APK contains duplicate class definitions");
            classes.addAll(dex.classes);
            natives.addAll(dex.natives);
            digest.update(names.get(i).getBytes(java.nio.charset.StandardCharsets.UTF_8));
            digest.update((byte) 0);
            digest.update(MessageDigest.getInstance("SHA-256").digest(dexes.get(i)));
        }
        return names.size() == 1 ? hex(MessageDigest.getInstance("SHA-256").digest(dexes.get(0))) : hex(digest.digest());
    }

    static String hex(byte[] bytes) {
        StringBuilder s = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) s.append(Character.forDigit((b >> 4) & 15, 16)).append(Character.forDigit(b & 15, 16));
        return s.toString();
    }

    private static String strip(String descriptor) {
        // Python descriptor[1:-1]
        return descriptor.length() < 2 ? "" : descriptor.substring(1, descriptor.length() - 1);
    }

    private int count(long offset) {
        long n = u32(offset);
        if (n > Integer.MAX_VALUE) throw new IllegalArgumentException(name + ": table too large");
        return (int) n;
    }

    private int index(long i, int size) {
        if (i < 0 || i >= size) throw new IndexOutOfBoundsException(name + ": index " + i + " out of range");
        return (int) i;
    }

    private long u32(long p) {
        if (p < 0 || p + 4 > data.length) throw new IndexOutOfBoundsException(name + ": read beyond DEX end");
        int q = (int) p;
        return (data[q] & 0xffL) | (data[q + 1] & 0xffL) << 8 | (data[q + 2] & 0xffL) << 16 | (data[q + 3] & 0xffL) << 24;
    }

    private int u16(long p) {
        if (p < 0 || p + 2 > data.length) throw new IndexOutOfBoundsException(name + ": read beyond DEX end");
        int q = (int) p;
        return (data[q] & 0xff) | (data[q + 1] & 0xff) << 8;
    }

    private int u8(long p) {
        if (p < 0 || p >= data.length) throw new IndexOutOfBoundsException(name + ": read beyond DEX end");
        return data[(int) p] & 0xff;
    }

    private long uleb(long[] cursor) {
        long value = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            int b = u8(cursor[0]++);
            value |= (long) (b & 127) << shift;
            if ((b & 128) == 0) return value;
        }
        throw new IllegalArgumentException(name + ": overlong ULEB128");
    }

    private List<String> typelist(long p, String[] types) {
        if (p == 0) return List.of();
        long size = u32(p);
        List<String> out = new ArrayList<>();
        for (long i = 0; i < size; ++i) out.add(types[index(u16(p + 4 + 2 * i), types.length)]);
        return out;
    }

    /** MUTF-8 (DEX string_data_item): C0 80 is NUL; surrogates are encoded individually. */
    private String string(long offset) {
        long[] cursor = {offset};
        long size = uleb(cursor);
        int p = (int) cursor[0];
        int end = p;
        while (true) {
            if (end >= data.length) throw new IllegalArgumentException(name + ": unterminated string");
            if (data[end] == 0) break;
            ++end;
        }
        StringBuilder s = new StringBuilder();
        int i = p;
        while (i < end) {
            int b0 = data[i] & 0xff;
            if (b0 == 0xc0 && i + 1 < end && (data[i + 1] & 0xff) == 0x80) { s.append('\0'); i += 2; continue; }
            if (b0 < 0x80) { s.append((char) b0); ++i; continue; }
            int need, cp, lo = 0x80, hi = 0xbf;
            if (b0 >= 0xc2 && b0 <= 0xdf) { need = 1; cp = b0 & 0x1f; }
            else if (b0 >= 0xe0 && b0 <= 0xef) {
                need = 2; cp = b0 & 0x0f;
                if (b0 == 0xe0) lo = 0xa0;
            } else if (b0 >= 0xf0 && b0 <= 0xf4) {
                need = 3; cp = b0 & 0x07;
                if (b0 == 0xf0) lo = 0x90;
                if (b0 == 0xf4) hi = 0x8f;
            } else throw new IllegalArgumentException(name + ": invalid modified UTF-8 string");
            if (i + need >= end) throw new IllegalArgumentException(name + ": invalid modified UTF-8 string");
            for (int k = 1; k <= need; ++k) {
                int b = data[i + k] & 0xff;
                if (b < (k == 1 ? lo : 0x80) || b > (k == 1 ? hi : 0xbf))
                    throw new IllegalArgumentException(name + ": invalid modified UTF-8 string");
                cp = cp << 6 | (b & 0x3f);
            }
            // UTF-8 surrogate encodings (ED A0..BF xx) are accepted, as Python's surrogatepass.
            s.appendCodePoint(cp);
            i += need + 1;
        }
        if (s.length() != size) throw new IllegalArgumentException(name + ": invalid modified UTF-8 string");
        return s.toString();
    }

    private static String bytesRepr(byte[] bytes) {
        StringBuilder s = new StringBuilder("b'");
        for (byte value : bytes) {
            int b = value & 0xff;
            if (b == '\\' || b == '\'') s.append('\\').append((char) b);
            else if (b == '\n') s.append("\\n");
            else if (b == '\r') s.append("\\r");
            else if (b == '\t') s.append("\\t");
            else if (b < 0x20 || b >= 0x7f) s.append(String.format(Locale.ROOT, "\\x%02x", b));
            else s.append((char) b);
        }
        return s.append('\'').toString();
    }
}
