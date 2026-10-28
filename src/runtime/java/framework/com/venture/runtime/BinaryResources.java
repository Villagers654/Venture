package com.venture.runtime;

import java.nio.*;
import java.nio.charset.StandardCharsets;
import java.util.*;

/**
 * Android ResChunk_header / ResStringPool_header, ResourceTypes.h (AOSP android-12.1).
 * Bounds are checked before following offsets: APK content is not trusted memory.
 */
public final class BinaryResources {
  final ByteBuffer bytes;
  public BinaryResources(byte[] data) {
    bytes = ByteBuffer.wrap(data).order(ByteOrder.LITTLE_ENDIAN);
  }
  int u16(int p) {
    check(p, 2);
    return Short.toUnsignedInt(bytes.getShort(p));
  }
  int i32(int p) {
    check(p, 4);
    return bytes.getInt(p);
  }
  int u8(int p) {
    check(p, 1);
    return Byte.toUnsignedInt(bytes.get(p));
  }
  void check(int p, int n) {
    if (p < 0 || n < 0 || p > bytes.limit() - n)
      throw new IllegalArgumentException("Truncated Android resource at " + p);
  }
  int end(int p) {
    int size = i32(p + 4);
    if (size < 8 || u16(p + 2) < 8 || u16(p + 2) > size)
      throw new IllegalArgumentException("Invalid resource chunk");
    check(p, size);
    return p + size;
  }
  String[] strings(int p) {
    int end = end(p), count = i32(p + 8), flags = i32(p + 16), start = i32(p + 20),
        header = u16(p + 2);
    if (count < 0 || count > (end - p - header) / 4)
      throw new IllegalArgumentException("Invalid string pool count");
    String[] out = new String[count];
    for (int i = 0; i < count; i++) {
      int at = p + start + i32(p + header + 4 * i);
      int[] cursor = {at};
      if ((flags & 256) != 0) {
        length8(cursor);
        int n = length8(cursor);
        check(cursor[0], n);
        if (cursor[0] + n >= end)
          throw new IllegalArgumentException("String outside pool");
        out[i] = new String(bytes.array(), cursor[0], n, StandardCharsets.UTF_8);
      } else {
        int n = u16(cursor[0]);
        cursor[0] += 2;
        if ((n & 0x8000) != 0) {
          n = ((n & 0x7fff) << 16) | u16(cursor[0]);
          cursor[0] += 2;
        }
        if (n < 0 || n > (end - cursor[0]) / 2)
          throw new IllegalArgumentException("String outside pool");
        out[i] = new String(bytes.array(), cursor[0], n * 2, StandardCharsets.UTF_16LE);
      }
    }
    return out;
  }
  int length8(int[] at) {
    int n = u8(at[0]++);
    if ((n & 128) != 0)
      n = ((n & 127) << 8) | u8(at[0]++);
    return n;
  }
  static String string(String[] pool, int index) {
    if (index == -1)
      return "";
    if (index < 0 || index >= pool.length)
      throw new IllegalArgumentException("Bad string index");
    return pool[index];
  }
  public record Value(int type, int data, String text) {
    public Object object() {
      return switch (type) {
        case 3 -> text;
        case 18 -> data != 0;
        default -> data;
      };
    }
    public String encoded() {
      return switch (type) {
        case 1 -> "@0x" + Integer.toHexString(data);
        case 2 -> "?0x" + Integer.toHexString(data);
        case 3 -> text;
        case 18 -> String.valueOf(data != 0);
        default -> String.valueOf(data);
      };
    }
  }
  public static final class Node {
    public final String name;
    public final Map<String, Value> attributes = new LinkedHashMap<>();
    public final List<Node> children = new ArrayList<>();
    public final List<Attribute> orderedAttributes = new ArrayList<>();
    public String text = "";
    Node(String name) {
      this.name = name;
    }
    public String text(String key, String fallback) {
      Value v = attributes.get(key);
      return v == null ? fallback : v.encoded();
    }
    public int integer(String key, int fallback) {
      Value v = attributes.get(key);
      return v == null ? fallback : v.data;
    }
    public Node child(String tag) {
      return children.stream().filter(n -> n.name.equals(tag)).findFirst().orElse(null);
    }
  }
  public record Attribute(String namespace, String name, int resourceId, Value value) {}
  public static Node xml(byte[] data) {
    BinaryResources r = new BinaryResources(data);
    if (r.u16(0) != 3)
      throw new IllegalArgumentException("Not Android binary XML");
    String[] pool = {};
    int[] resourceIds = {};
    Deque<Node> stack = new ArrayDeque<>();
    Node root = null;
    for (int p = r.u16(2), end = r.end(0); p < end;) {
      int next = r.end(p), type = r.u16(p);
      if (type == 1)
        pool = r.strings(p);
      else if (type == 0x180) {
        int count = (next - p - r.u16(p + 2)) / 4;
        resourceIds = new int[count];
        for (int i = 0; i < count; i++) resourceIds[i] = r.i32(p + r.u16(p + 2) + i * 4);
      } else if (type == 0x102) {
        int ext = p + r.u16(p + 2);
        Node node = new Node(string(pool, r.i32(ext + 4)));
        int count = r.u16(ext + 12), size = r.u16(ext + 10), start = ext + r.u16(ext + 8);
        if (size < 20 || start < ext || count > (next - start) / size)
          throw new IllegalArgumentException("Bad XML attributes");
        for (int i = 0; i < count; i++) {
          int a = start + i * size, t = r.u8(a + 15), v = r.i32(a + 16);
          String raw = string(pool, r.i32(a + 8));
          int nameIndex = r.i32(a + 4);
          String name = string(pool, nameIndex);
          Value value = new Value(t, v, t == 3 ? string(pool, v) : raw);
          node.attributes.put(name, value);
          node.orderedAttributes.add(new Attribute(string(pool, r.i32(a)), name,
              nameIndex < resourceIds.length ? resourceIds[nameIndex] : 0, value));
        }
        if (stack.isEmpty()) {
          if (root != null)
            throw new IllegalArgumentException("Multiple XML roots");
          root = node;
        } else
          stack.peek().children.add(node);
        stack.push(node);
      } else if (type == 0x103) {
        if (stack.isEmpty())
          throw new IllegalArgumentException("Unbalanced XML");
        stack.pop();
      } else if (type == 0x104 && !stack.isEmpty())
        stack.peek().text += string(pool, r.i32(p + r.u16(p + 2)));
      p = next;
    }
    if (root == null || !stack.isEmpty())
      throw new IllegalArgumentException("Unclosed XML");
    return root;
  }
}
