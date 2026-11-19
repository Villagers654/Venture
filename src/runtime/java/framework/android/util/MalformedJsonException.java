package android.util;

import java.io.IOException;

/** Thrown by JsonReader for input that is not valid JSON (or not even lenient JSON). */
public final class MalformedJsonException extends IOException {
  private static final long serialVersionUID = 1L;
  public MalformedJsonException(String message) {
    super(message);
  }
}
