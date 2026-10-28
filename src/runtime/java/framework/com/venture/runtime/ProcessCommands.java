package com.venture.runtime;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.List;

/** Android commands backed by Venture services rather than host executables. */
public final class ProcessCommands {
  private ProcessCommands() {}
  private record Entry(long sequence, byte[] bytes) {}
  private static final ArrayDeque<Entry> logs = new ArrayDeque<>();
  private static long sequence;
  private static int bytes;

  public static synchronized void log(int priority, String tag, String text) {
    byte[] line = ("VDIWEF".charAt(Math.max(0, Math.min(5, priority - 2))) + "/" + tag
        + "(" + android.os.Process.myPid() + "): " + text + "\n").getBytes(StandardCharsets.UTF_8);
    logs.addLast(new Entry(++sequence, line));
    bytes += line.length;
    while (bytes > 256 * 1024 && logs.size() > 1) bytes -= logs.removeFirst().bytes.length;
    ProcessCommands.class.notifyAll();
  }

  public static Process start(ProcessBuilder builder) throws IOException {
    List<String> command = builder.command();
    if (command.isEmpty()) return builder.start();
    String program = command.get(0);
    if (!program.equals("logcat") && !program.equals("/system/bin/logcat")) return builder.start();
    RuntimeEnvironment.host().log(4, "process", "Starting Android logcat: " + command);
    synchronized (ProcessCommands.class) {
      if (command.contains("-c")) { logs.clear(); bytes = 0; }
      return new LogProcess(command.contains("-d") || command.contains("-c"));
    }
  }

  private static final class LogProcess extends Process {
    private boolean exited;
    private long next;
    private byte[] pending = new byte[0];
    private int offset;
    private final long end;
    private final InputStream output = new InputStream() {
      @Override public int read(byte[] buffer, int start, int length) throws IOException {
        java.util.Objects.checkFromIndexSize(start, length, buffer.length);
        if (length == 0) return 0;
        int first = read();
        if (first < 0) return -1;
        buffer[start] = (byte) first;
        synchronized (ProcessCommands.class) {
          int count = Math.min(length - 1, pending.length - offset);
          System.arraycopy(pending, offset, buffer, start + 1, count);
          offset += count;
          return count + 1;
        }
      }
      @Override public int read() throws IOException {
        synchronized (ProcessCommands.class) {
          for (;;) {
            if (offset < pending.length) return pending[offset++] & 255;
            if (exited) return -1;
            for (Entry entry : logs) if (entry.sequence >= next && entry.sequence <= end) {
              next = entry.sequence + 1; pending = entry.bytes; offset = 0; break;
            }
            if (offset < pending.length) continue;
            if (end != Long.MAX_VALUE) { destroy(); return -1; }
            try { ProcessCommands.class.wait(); }
            catch (InterruptedException e) { Thread.currentThread().interrupt(); throw new InterruptedIOException(); }
          }
        }
      }
      @Override public void close() { destroy(); }
    };
    LogProcess(boolean dump) {
      next = logs.isEmpty() ? sequence + 1 : logs.getFirst().sequence;
      end = dump ? sequence : Long.MAX_VALUE;
      if (dump) {
        ByteArrayOutputStream snapshot = new ByteArrayOutputStream();
        for (Entry entry : logs) snapshot.writeBytes(entry.bytes);
        pending = snapshot.toByteArray();
        exited = true;
      }
    }
    @Override public InputStream getInputStream() { return output; }
    @Override public InputStream getErrorStream() { return InputStream.nullInputStream(); }
    @Override public OutputStream getOutputStream() { return OutputStream.nullOutputStream(); }
    @Override public int waitFor() throws InterruptedException {
      synchronized (ProcessCommands.class) { while (!exited) ProcessCommands.class.wait(); return 0; }
    }
    @Override public int exitValue() {
      synchronized (ProcessCommands.class) { if (!exited) throw new IllegalThreadStateException(); return 0; }
    }
    @Override public void destroy() {
      synchronized (ProcessCommands.class) { exited = true; ProcessCommands.class.notifyAll(); }
    }
  }
}
