package android.net;

import java.io.Closeable;
import java.io.FileDescriptor;
import java.io.IOException;

// AOSP LocalServerSocket: Venture has no local socket namespace (LocalSocket), so
// binding a name fails with IOException as Android does when the bind is refused.
public class LocalServerSocket implements Closeable {
  public LocalServerSocket(String name) throws IOException {
    throw new IOException("Address not available");
  }
  public LocalServerSocket(FileDescriptor fd) throws IOException {
    throw new IOException("Address not available");
  }
  public LocalSocketAddress getLocalSocketAddress() {
    return null;
  }
  public LocalSocket accept() throws IOException {
    throw new IOException("socket closed");
  }
  public FileDescriptor getFileDescriptor() {
    return null;
  }
  @Override
  public void close() throws IOException {}
}
