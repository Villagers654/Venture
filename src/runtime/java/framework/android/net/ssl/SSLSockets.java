package android.net.ssl;

import javax.net.ssl.SSLSocket;

/**
 * Android 10's TLS socket extensions (OkHttp's Android10SocketAdapter). Every SSLSocket the
 * platform creates is supported. The host JDK already offers session tickets on resumption,
 * so the ticket switch has nothing left to change.
 */
public final class SSLSockets {
  private SSLSockets() {}

  public static boolean isSupportedSocket(SSLSocket socket) {
    return socket != null;
  }

  public static void setUseSessionTickets(SSLSocket socket, boolean useSessionTickets) {
    if (!isSupportedSocket(socket)) throw new IllegalArgumentException("Socket is not a platform SSL socket");
  }
}
