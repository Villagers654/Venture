package com.venture.runtime;

import android.content.res.Configuration;
import android.util.DisplayMetrics;
import java.util.*;

/**
 * ResTable_type / entry / map from AOSP ResourceTypes.h, android-12.1.
 * Supports ordinary, offset16 and sparse entry tables, reference chains and bags.
 */
public final class ResourceTable {
  public record Entry(String packageName, String type, String name, BinaryResources.Value value,
      int parent, Map<Integer, BinaryResources.Value> bag, byte[] config) {}
  private final Map<Integer, List<Entry>> entries = new HashMap<>();
  private final Map<String, Integer> names = new HashMap<>();
  public ResourceTable(byte[] data) {
    if (data == null || data.length == 0)
      return;
    BinaryResources r = new BinaryResources(data);
    if (r.u16(0) != 2)
      throw new IllegalArgumentException("Not resources.arsc");
    String[] global = {};
    for (int p = r.u16(2), end = r.end(0); p < end; p = r.end(p)) {
      if (r.u16(p) == 1)
        global = r.strings(p);
      if (r.u16(p) == 0x200)
        readPackage(r, p, global);
    }
  }
  private void readPackage(BinaryResources r, int p, String[] global) {
    int header = r.u16(p + 2), id = r.i32(p + 8), end = r.end(p);
    r.check(p + 12, 256);
    String pkg =
        new String(r.bytes.array(), p + 12, 256, java.nio.charset.StandardCharsets.UTF_16LE)
            .split("\u0000", 2)[0];
    String[] types = r.strings(p + r.i32(p + 268)), keys = r.strings(p + r.i32(p + 276));
    int typeOffset = header >= 288 ? r.i32(p + 284) : 0;
    for (int t = p + header; t < end; t = r.end(t))
      if (r.u16(t) == 0x201) {
        int typeId = r.u8(t + 8), flags = r.u8(t + 9), count = r.i32(t + 12),
            entryStart = t + r.i32(t + 16), offsets = t + r.u16(t + 2), limit = r.end(t),
            configSize = r.i32(t + 20);
        if (configSize < 28 || configSize > r.u16(t + 2) - 20 || count < 0
            || count > (limit - offsets) / ((flags & 2) != 0 ? 2 : 4))
          throw new IllegalArgumentException("Invalid ARSC type");
        byte[] config = Arrays.copyOfRange(r.bytes.array(), t + 20, t + 20 + configSize);
        for (int n = 0; n < count; n++) {
          int index = n, off;
          if ((flags & 1) != 0) {
            index = r.u16(offsets + 4 * n);
            off = r.u16(offsets + 4 * n + 2) * 4;
          } else if ((flags & 2) != 0) {
            off = r.u16(offsets + 2 * n);
            if (off == 65535)
              continue;
            off *= 4;
          } else {
            off = r.i32(offsets + 4 * n);
            if (off == -1)
              continue;
          }
          int e = entryStart + off;
          if (e < entryStart || e > limit - 8)
            throw new IllegalArgumentException("Entry outside ARSC type");
          int size = r.u16(e), eflags = r.u16(e + 2), key = r.i32(e + 4);
          if (size < 8 || size > limit - e)
            throw new IllegalArgumentException("Invalid ARSC entry");
          BinaryResources.Value value = null;
          int parent = 0;
          Map<Integer, BinaryResources.Value> bag = new LinkedHashMap<>();
          if ((eflags & 1) != 0) {
            parent = r.i32(e + 8);
            int maps = r.i32(e + 12);
            if (size < 16 || maps < 0 || maps > (limit - e - size) / 12)
              throw new IllegalArgumentException("Invalid ARSC bag");
            for (int m = 0; m < maps; m++) {
              int at = e + size + 12 * m;
              bag.put(r.i32(at), value(r, at + 4, global));
            }
          } else
            value = value(r, e + size, global);
          String type = BinaryResources.string(types, typeId - 1),
                 name = BinaryResources.string(keys, key);
          int resource = (id << 24) | ((typeId + typeOffset) << 16) | index;
          Entry entry = new Entry(pkg, type, name, value, parent, Map.copyOf(bag), config);
          entries.computeIfAbsent(resource, k -> new ArrayList<>()).add(entry);
          names.put(pkg + ":" + type + "/" + name, resource);
        }
      }
  }
  private static BinaryResources.Value value(BinaryResources r, int p, String[] strings) {
    int type = r.u8(p + 3), data = r.i32(p + 4);
    return new BinaryResources.Value(
        type, data, type == 3 ? BinaryResources.string(strings, data) : null);
  }
  private static int u16(byte[] c, int n) {
    return n + 1 < c.length ? (c[n] & 255) | ((c[n + 1] & 255) << 8) : 0;
  }
  private static int u8(byte[] c, int n) {
    return n < c.length ? c[n] & 255 : 0;
  }
  private static long score(byte[] c, Configuration config, DisplayMetrics metrics) {
    int sdk = u16(c, 24);
    if (sdk > android.os.Build.VERSION.SDK_INT)
      return Long.MIN_VALUE;
    String language = "" + (char) u8(c, 8) + (char) u8(c, 9);
    String country = "" + (char) u8(c, 10) + (char) u8(c, 11);
    long score = sdk;
    if (u8(c, 8) != 0) {
      if (!language.equals(config.locale.getLanguage()))
        return Long.MIN_VALUE;
      score += 1L << 40;
    }
    if (u8(c, 10) != 0) {
      if (!country.equals(config.locale.getCountry()))
        return Long.MIN_VALUE;
      score += 1L << 38;
    }
    int orientation = u8(c, 12);
    if (orientation != 0 && orientation != config.orientation)
      return Long.MIN_VALUE;
    int night = u8(c, 29) & 0x30;
    if (night != 0 && night != (config.uiMode & 0x30))
      return Long.MIN_VALUE;
    int sw = u16(c, 30), w = u16(c, 32), h = u16(c, 34);
    if (sw > config.smallestScreenWidthDp || w > config.screenWidthDp || h > config.screenHeightDp)
      return Long.MIN_VALUE;
    score += ((long) sw << 24) + ((long) w << 16) + ((long) h << 8);
    if (orientation != 0)
      score += 1L << 35;
    if (night != 0)
      score += 1L << 34;
    int density = u16(c, 14);
    if (density == 0)
      density = 160;
    score +=
        density == 65534 ? 1L << 33 : Math.max(0, 10000 - Math.abs(density - metrics.densityDpi));
    return score;
  }
  public Entry entry(int id, Configuration c, DisplayMetrics d) {
    List<Entry> variants = entries.get(id);
    if (variants == null)
      throw new android.content.res.Resources.NotFoundException(
          "Resource 0x" + Integer.toHexString(id));
    Entry best = null;
    long highest = Long.MIN_VALUE;
    for (Entry e : variants) {
      long s = score(e.config, c, d);
      if (s > highest) {
        best = e;
        highest = s;
      }
    }
    if (best == null)
      throw new android.content.res.Resources.NotFoundException(
          "No matching configuration for 0x" + Integer.toHexString(id));
    return best;
  }
  public BinaryResources.Value resolve(int id, Configuration c, DisplayMetrics d) {
    Set<Integer> seen = new HashSet<>();
    for (;;) {
      if (!seen.add(id))
        throw new IllegalArgumentException("Cyclic resource reference");
      BinaryResources.Value v = entry(id, c, d).value;
      if (v == null)
        throw new android.content.res.Resources.NotFoundException("Resource is a bag");
      if (v.type() != 1)
        return v;
      if (v.data() == 0)
        return new BinaryResources.Value(0, 0, null);
      id = v.data();
    }
  }
  public BinaryResources.Value nonConfigurationValue(int id) {
    var variants = entries.get(id);
    if (variants == null) throw new android.content.res.Resources.NotFoundException("Manifest resource");
    // Manifest data attributes cannot vary with device configuration (AOSP
    // TypedArray.getNonConfigurationString with allowedChangingConfigs=0).
    // Conservatively reject qualified entries until type-spec masks are modeled.
    for (var entry : variants) for (int i = 4; i < entry.config.length; i++)
      if (entry.config[i] != 0) throw new IllegalArgumentException("Configuration-dependent manifest resource");
    var value = variants.get(0).value;
    if (value == null) throw new IllegalArgumentException("Manifest resource is a bag");
    return value;
  }
  public int identifier(String name, String type, String pkg) {
    if (!name.contains(":"))
      name = pkg + ":" + name;
    if (!name.contains("/"))
      name = name.substring(0, name.indexOf(':') + 1) + type + "/"
          + name.substring(name.indexOf(':') + 1);
    return names.getOrDefault(name, 0);
  }
}
