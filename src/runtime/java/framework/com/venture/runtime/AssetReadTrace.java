package com.venture.runtime;

import java.io.*;
import java.lang.management.ManagementFactory;

/** Optional HotSpot counters for actual Java asset opens; no asset contents logged. */
public final class AssetReadTrace {
  private AssetReadTrace() {}
  private static final boolean enabled = HostEnvironment.getenv("VENTURE_TRACE_ASSETS") != null;
  private static final java.lang.management.MemoryMXBean heap = enabled ? ManagementFactory.getMemoryMXBean() : null;
  private static final com.sun.management.ThreadMXBean allocations = enabled
      && ManagementFactory.getThreadMXBean() instanceof com.sun.management.ThreadMXBean bean
      && bean.isThreadAllocatedMemorySupported() && bean.isThreadAllocatedMemoryEnabled() ? bean : null;

  private static long allocated() {
    return allocations == null ? -1 : allocations.getThreadAllocatedBytes(Thread.currentThread().threadId());
  }
  public static final class Open {
    private final String file;
    private final long initialHeap, initialAllocated, begin;
    private long openNs, openAllocated, firstByteNs = -1, peakHeap, bytes;
    private boolean reportedFirst, reportedClose;
    private java.security.MessageDigest readDigest;
    private void digest(byte[] data, int offset, int count) {
      if (count <= 0) return;
      if (readDigest == null) {
        try { readDigest = java.security.MessageDigest.getInstance("SHA-256"); }
        catch (java.security.NoSuchAlgorithmException impossible) { throw new AssertionError(impossible); }
      }
      readDigest.update(data, offset, count);
    }
    private Open(String path) {
      file = Integer.toHexString(path.hashCode());
      initialHeap = peakHeap = heap.getHeapMemoryUsage().getUsed();
      initialAllocated = allocated();
      begin = System.nanoTime();
    }
    private void sample() { peakHeap = Math.max(peakHeap, heap.getHeapMemoryUsage().getUsed()); }
    private void read(int count) {
      if (count > 0) {
        bytes += count;
        if (firstByteNs < 0) firstByteNs = System.nanoTime() - begin;
      }
      sample();
      if (!reportedFirst && count != 0) { reportedFirst = true; report("first"); }
    }
    private void report(String phase) {
      RuntimeEnvironment.host().log(4, "Assets", "phase=" + phase + " file=" + file
          + " open_ns=" + openNs + " first_byte_ns=" + firstByteNs
          + " open_allocated_bytes=" + openAllocated + " heap_before_bytes=" + initialHeap
          + " observed_peak_heap_bytes=" + peakHeap + " read_bytes=" + bytes
          + (phase.equals("close") && readDigest != null
              ? " read_sha256=" + java.util.HexFormat.of().formatHex(readDigest.digest()) : ""));
    }
  }
  public static Open begin(String path) { return enabled ? new Open(path) : null; }
  public static InputStream opened(InputStream stream, Open open) {
    if (open == null) return stream;
    open.openNs = System.nanoTime() - open.begin;
    long now = allocated();
    open.openAllocated = now < 0 || open.initialAllocated < 0 ? -1 : now - open.initialAllocated;
    open.sample();
    // Use InputStream defaults for transferTo/readAllBytes so those operations
    // also pass through read(), unlike FilterInputStream's delegate shortcuts.
    return new InputStream() {
      public int read() throws IOException {
        int result = stream.read();
        open.read(result < 0 ? -1 : 1);
        if (result >= 0) open.digest(new byte[]{(byte)result}, 0, 1);
        return result;
      }
      public int read(byte[] bytes, int offset, int length) throws IOException {
        int result = stream.read(bytes, offset, length);
        open.read(result);
        open.digest(bytes, offset, result);
        return result;
      }
      public long skip(long count) throws IOException { return stream.skip(count); }
      public int available() throws IOException { return stream.available(); }
      public boolean markSupported() { return stream.markSupported(); }
      public void mark(int limit) { stream.mark(limit); }
      public void reset() throws IOException { stream.reset(); }
      public void close() throws IOException {
        try { stream.close(); }
        finally {
          if (!open.reportedClose) {
            open.reportedClose = true;
            open.sample();
            open.report("close");
          }
        }
      }
    };
  }
}
