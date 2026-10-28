package android.system;

import java.io.IOException;
import java.net.SocketException;

// AOSP libcore android/system/ErrnoException.java: checked Linux errno carrier;
// both rethrow helpers always throw and retain this exception as their cause.
public final class ErrnoException extends Exception {
  private final String functionName;
  public final int errno;

  public ErrnoException(String functionName, int errno) {
    this.functionName = functionName;
    this.errno = errno;
  }

  public ErrnoException(String functionName, int errno, Throwable cause) {
    super(cause);
    this.functionName = functionName;
    this.errno = errno;
  }

  @Override public String getMessage() {
    return functionName + " failed: errno " + errno + " (" + Os.strerror(errno) + ")";
  }

  public IOException rethrowAsIOException() throws IOException {
    throw new IOException(getMessage(), this);
  }

  public SocketException rethrowAsSocketException() throws SocketException {
    var error = new SocketException(getMessage());
    error.initCause(this);
    throw error;
  }
}
