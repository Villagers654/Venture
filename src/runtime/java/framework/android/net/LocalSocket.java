package android.net;

import java.io.Closeable;
import java.io.FileDescriptor;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

// AOSP LocalSocket: an AF_UNIX socket. No local service of a Quest listens in
// Venture's process (Vuplex WebView probes one from a background task, and On
// Target VR Darts stopped on the missing class), so connect fails as it does on
// Android when nothing is bound to the name: IOException, connection refused.
public class LocalSocket implements Closeable {
  public static final int SOCKET_DGRAM = 1;
  public static final int SOCKET_STREAM = 2;
  public static final int SOCKET_SEQPACKET = 3;
  private final int type;
  private boolean closed;
  private int timeout, receiveBufferSize = 65536, sendBufferSize = 65536;
  public LocalSocket() {
    this(SOCKET_STREAM);
  }
  public LocalSocket(int sockType) {
    if (sockType != SOCKET_DGRAM && sockType != SOCKET_STREAM && sockType != SOCKET_SEQPACKET)
      throw new IllegalArgumentException("unknown sockType");
    type = sockType;
  }
  public void connect(LocalSocketAddress endpoint) throws IOException {
    if (closed) throw new IOException("socket closed");
    throw new IOException("Connection refused");
  }
  public void connect(LocalSocketAddress endpoint, int timeout) throws IOException {
    connect(endpoint);
  }
  public void bind(LocalSocketAddress bindpoint) throws IOException {
    if (closed) throw new IOException("socket closed");
    throw new IOException("Address not available");
  }
  public LocalSocketAddress getLocalSocketAddress() {
    return null;
  }
  public LocalSocketAddress getRemoteSocketAddress() {
    return null;
  }
  public InputStream getInputStream() throws IOException {
    throw new IOException("socket not connected");
  }
  public OutputStream getOutputStream() throws IOException {
    throw new IOException("socket not connected");
  }
  @Override
  public void close() throws IOException {
    closed = true;
  }
  public void shutdownInput() throws IOException {
    throw new IOException("socket not connected");
  }
  public void shutdownOutput() throws IOException {
    throw new IOException("socket not connected");
  }
  public void setReceiveBufferSize(int size) throws IOException {
    receiveBufferSize = size;
  }
  public int getReceiveBufferSize() throws IOException {
    return receiveBufferSize;
  }
  public void setSoTimeout(int n) throws IOException {
    timeout = n;
  }
  public int getSoTimeout() throws IOException {
    return timeout;
  }
  public void setSendBufferSize(int n) throws IOException {
    sendBufferSize = n;
  }
  public int getSendBufferSize() throws IOException {
    return sendBufferSize;
  }
  public boolean isBound() {
    return false;
  }
  public boolean isClosed() {
    return closed;
  }
  public boolean isConnected() {
    return false;
  }
  public boolean isInputShutdown() {
    return false;
  }
  public boolean isOutputShutdown() {
    return false;
  }
  public FileDescriptor getFileDescriptor() {
    return null;
  }
  @Override
  public String toString() {
    return super.toString() + " type=" + type;
  }
}
