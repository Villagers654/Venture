package android.os;

import java.io.IOException;
import java.nio.file.FileStore;
import java.nio.file.Files;
import java.nio.file.Path;

/**
 * AOSP frameworks/base/core/java/android/os/StatFs.java: statvfs(2) of the file
 * system holding a path, sampled at construction and restat(). The host file
 * store supplies the block size and total, free and caller-available bytes.
 */
public class StatFs {
  private long blockSize, totalBytes, freeBytes, availableBytes;
  public StatFs(String path) { restat(path); }
  public void restat(String path) {
    try {
      FileStore store = Files.getFileStore(Path.of(path));
      long block = store.getBlockSize();
      blockSize = block > 0 ? block : 4096;
      totalBytes = store.getTotalSpace();
      // statvfs is one snapshot with f_bavail <= f_bfree <= f_blocks; the host reads
      // these as separate queries, so writes elsewhere on the volume can reorder them.
      freeBytes = Math.min(store.getUnallocatedSpace(), totalBytes);
      availableBytes = Math.min(store.getUsableSpace(), freeBytes);
    } catch (IOException | RuntimeException e) {
      // StatFs.doStat: an ErrnoException from statvfs becomes IllegalArgumentException.
      throw new IllegalArgumentException("Invalid path: " + path, e);
    }
  }
  public long getBlockSizeLong() { return blockSize; }
  public long getBlockCountLong() { return totalBytes / blockSize; }
  public long getFreeBlocksLong() { return freeBytes / blockSize; }
  public long getAvailableBlocksLong() { return availableBytes / blockSize; }
  public long getFreeBytes() { return getFreeBlocksLong() * blockSize; }
  public long getAvailableBytes() { return getAvailableBlocksLong() * blockSize; }
  public long getTotalBytes() { return getBlockCountLong() * blockSize; }
  // The deprecated int accessors truncate like AOSP's (int) casts.
  @Deprecated public int getBlockSize() { return (int) getBlockSizeLong(); }
  @Deprecated public int getBlockCount() { return (int) getBlockCountLong(); }
  @Deprecated public int getFreeBlocks() { return (int) getFreeBlocksLong(); }
  @Deprecated public int getAvailableBlocks() { return (int) getAvailableBlocksLong(); }
}
