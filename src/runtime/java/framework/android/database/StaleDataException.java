package android.database;

/** AOSP: data read from a cursor whose window was released. */
public class StaleDataException extends RuntimeException {
  public StaleDataException() {}
  public StaleDataException(String description) { super(description); }
}
