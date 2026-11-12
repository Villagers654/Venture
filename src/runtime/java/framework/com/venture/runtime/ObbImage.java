package com.venture.runtime;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;

/**
 * An OBB filesystem image (AOSP ObbFile.cpp footer after a FAT12/16/32 volume), which Android's
 * vold mounts for StorageManager.mountObb. Here the volume's files are unpacked once into a
 * directory, which stands as the mount. Encrypted (salted) images are not unpacked.
 */
public final class ObbImage {
  private static final int SIGNATURE = 0x01059983, FLAG_SALTED = 2;
  private ObbImage() {}

  /** The FAT volume's size in bytes, or -1 if the file is not an unencrypted OBB image. */
  static long volumeSize(RandomAccessFile file) throws IOException {
    long length = file.length();
    if (length < 8 + 24) return -1;
    file.seek(length - 8);
    ByteBuffer tail = read(file, length - 8, 8);
    long footer = tail.getInt() & 0xffffffffL;
    if (tail.getInt() != SIGNATURE || footer < 24 || footer > length - 8) return -1;
    ByteBuffer info = read(file, length - 8 - footer, 12);
    info.getInt(); // footer version
    info.getInt(); // package version
    if ((info.getInt() & FLAG_SALTED) != 0) return -1;
    return length - 8 - footer;
  }

  /**
   * Unpacks the image's files under `into` (replacing what an earlier, different image left
   * there) and returns it, or null if `obb` is not an unencrypted OBB image of a FAT volume.
   */
  public static synchronized File unpack(File obb, File into) throws IOException {
    try (var file = new RandomAccessFile(obb, "r")) {
      long size = volumeSize(file);
      if (size <= 0) return null;
      String stamp = obb.length() + ":" + obb.lastModified();
      File marker = new File(into, ".venture-obb");
      if (marker.isFile() && Files.readString(marker.toPath()).equals(stamp)) return into;
      var volume = new Volume(file, size);
      File staging = new File(into.getParentFile(), into.getName() + ".unpacking");
      delete(staging);
      if (!staging.mkdirs()) throw new IOException("Cannot create " + staging);
      volume.extract(volume.root(), staging, 0);
      Files.writeString(new File(staging, ".venture-obb").toPath(), stamp);
      delete(into);
      Files.move(staging.toPath(), into.toPath(), StandardCopyOption.ATOMIC_MOVE);
      return into;
    }
  }

  private static ByteBuffer read(RandomAccessFile file, long offset, int length) throws IOException {
    byte[] bytes = new byte[length];
    file.seek(offset);
    file.readFully(bytes);
    return ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
  }
  private static void delete(File f) throws IOException {
    var path = f.toPath();
    if (!Files.exists(path, java.nio.file.LinkOption.NOFOLLOW_LINKS)) return;
    // A junction or symlink is unlinked, never followed.
    var attributes = Files.readAttributes(path, java.nio.file.attribute.BasicFileAttributes.class,
                                          java.nio.file.LinkOption.NOFOLLOW_LINKS);
    if (attributes.isDirectory() && !attributes.isOther()) {
      File[] children = f.listFiles();
      if (children != null) for (File c : children) delete(c);
    }
    Files.delete(path);
  }

  private static final class Volume {
    final RandomAccessFile file;
    final long size;
    final int bytesPerSector, clusterBytes, type, rootEntries;
    final long fatOffset, rootOffset, dataOffset, clusters, rootCluster;
    final byte[] fat;
    Volume(RandomAccessFile file, long size) throws IOException {
      this.file = file;
      this.size = size;
      ByteBuffer boot = read(file, 0, 512);
      bytesPerSector = boot.getShort(11) & 0xffff;
      int sectorsPerCluster = boot.get(13) & 0xff;
      int reserved = boot.getShort(14) & 0xffff;
      int fats = boot.get(16) & 0xff;
      rootEntries = boot.getShort(17) & 0xffff;
      long totalSectors = (boot.getShort(19) & 0xffff) != 0 ? boot.getShort(19) & 0xffff : boot.getInt(32) & 0xffffffffL;
      long fatSectors = (boot.getShort(22) & 0xffff) != 0 ? boot.getShort(22) & 0xffff : boot.getInt(36) & 0xffffffffL;
      if (Integer.bitCount(bytesPerSector) != 1 || bytesPerSector < 512 || sectorsPerCluster == 0 || fats == 0 ||
          fatSectors == 0 || (boot.getShort(510) & 0xffff) != 0xaa55)
        throw new IOException("Not a FAT volume");
      clusterBytes = bytesPerSector * sectorsPerCluster;
      long rootSectors = ((long) rootEntries * 32 + bytesPerSector - 1) / bytesPerSector;
      fatOffset = (long) reserved * bytesPerSector;
      rootOffset = fatOffset + fats * fatSectors * bytesPerSector;
      dataOffset = rootOffset + rootSectors * bytesPerSector;
      clusters = (totalSectors - dataOffset / bytesPerSector) / sectorsPerCluster;
      type = clusters < 4085 ? 12 : clusters < 65525 ? 16 : 32;
      rootCluster = type == 32 ? boot.getInt(44) & 0x0fffffffL : 0;
      long fatBytes = fatSectors * bytesPerSector;
      if (fatBytes > Integer.MAX_VALUE || dataOffset > size) throw new IOException("Not a FAT volume");
      fat = new byte[(int) fatBytes];
      file.seek(fatOffset);
      file.readFully(fat);
    }
    long next(long cluster) {
      ByteBuffer f = ByteBuffer.wrap(fat).order(ByteOrder.LITTLE_ENDIAN);
      switch (type) {
        case 12: {
          int at = (int) (cluster + cluster / 2);
          if (at + 1 >= fat.length) return -1;
          int v = f.getShort(at) & 0xffff;
          v = (cluster & 1) != 0 ? v >> 4 : v & 0xfff;
          return v >= 0xff8 ? -1 : v;
        }
        case 16: {
          if (cluster * 2 + 1 >= fat.length) return -1;
          int v = f.getShort((int) cluster * 2) & 0xffff;
          return v >= 0xfff8 ? -1 : v;
        }
        default: {
          if (cluster * 4 + 3 >= fat.length) return -1;
          long v = f.getInt((int) cluster * 4) & 0x0fffffffL;
          return v >= 0x0ffffff8L ? -1 : v;
        }
      }
    }
    /** The bytes of a cluster chain (a directory), at most `limit`. */
    byte[] chain(long cluster, long limit) throws IOException {
      var out = new java.io.ByteArrayOutputStream();
      for (long n = 0; cluster >= 2 && cluster < clusters + 2 && out.size() < limit; n++) {
        if (n > clusters) throw new IOException("FAT cluster chain loops");
        out.write(read(file, dataOffset + (cluster - 2) * clusterBytes, clusterBytes).array());
        cluster = next(cluster);
      }
      return out.toByteArray();
    }
    byte[] root() throws IOException {
      return type == 32 ? chain(rootCluster, 64L << 20) : read(file, rootOffset, rootEntries * 32).array();
    }
    void extract(byte[] directory, File into, int depth) throws IOException {
      if (depth > 64) throw new IOException("FAT directories nest too deep");
      ByteBuffer d = ByteBuffer.wrap(directory).order(ByteOrder.LITTLE_ENDIAN);
      char[] longName = new char[260];
      int longLength = -1;
      for (int at = 0; at + 32 <= directory.length; at += 32) {
        int first = directory[at] & 0xff;
        if (first == 0) break;
        int attributes = directory[at + 11] & 0xff;
        if (first == 0xe5) { longLength = -1; continue; }
        if (attributes == 0x0f) {
          int sequence = first & 0x1f;
          if ((first & 0x40) != 0) longLength = 0;
          if (sequence == 0 || sequence > 20 || longLength < 0) { longLength = -1; continue; }
          int[] offsets = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
          for (int i = 0; i < offsets.length; i++) {
            char c = d.getChar(at + offsets[i]);
            if (c == 0 || c == 0xffff) break;
            int index = (sequence - 1) * 13 + i;
            longName[index] = c;
            longLength = Math.max(longLength, index + 1);
          }
          continue;
        }
        String name = longLength > 0 ? new String(longName, 0, longLength) : shortName(directory, at);
        longLength = -1;
        if ((attributes & 0x08) != 0 || name.equals(".") || name.equals("..")) continue;
        if (name.isEmpty() || name.contains("/") || name.contains("\\") || name.equals(".."))
          throw new IOException("Invalid FAT file name " + name);
        long cluster = ((long) (d.getShort(at + 20) & 0xffff) << 16) | (d.getShort(at + 26) & 0xffff);
        File target = new File(into, RuntimeEnvironment.hostName(name));
        if ((attributes & 0x10) != 0) {
          if (!target.mkdirs() && !target.isDirectory()) throw new IOException("Cannot create " + target);
          extract(chain(cluster, 64L << 20), target, depth + 1);
        } else {
          copy(cluster, d.getInt(at + 28) & 0xffffffffL, target);
        }
      }
    }
    void copy(long cluster, long length, File target) throws IOException {
      try (var out = new java.io.FileOutputStream(target)) {
        byte[] buffer = new byte[clusterBytes];
        for (long n = 0; length > 0; n++) {
          if (cluster < 2 || cluster >= clusters + 2 || n > clusters) throw new IOException("Truncated FAT file " + target);
          int part = (int) Math.min(length, clusterBytes);
          file.seek(dataOffset + (cluster - 2) * clusterBytes);
          file.readFully(buffer, 0, part);
          out.write(buffer, 0, part);
          length -= part;
          cluster = next(cluster);
        }
      }
    }
    static String shortName(byte[] directory, int at) {
      int flags = directory[at + 12] & 0xff;
      String base = new String(directory, at, 8, StandardCharsets.ISO_8859_1).stripTrailing();
      String extension = new String(directory, at + 8, 3, StandardCharsets.ISO_8859_1).stripTrailing();
      if ((directory[at] & 0xff) == 0x05) base = (char) 0xe5 + base.substring(1);
      if ((flags & 0x08) != 0) base = base.toLowerCase(java.util.Locale.ROOT);
      if ((flags & 0x10) != 0) extension = extension.toLowerCase(java.util.Locale.ROOT);
      return extension.isEmpty() ? base : base + "." + extension;
    }
  }
}
