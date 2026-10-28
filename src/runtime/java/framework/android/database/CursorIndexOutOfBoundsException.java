package android.database;

/** AOSP: a cursor read at a position outside its rows. */
public class CursorIndexOutOfBoundsException extends IndexOutOfBoundsException {
  public CursorIndexOutOfBoundsException(int index, int size) {
    super("Index " + index + " requested, with a size of " + size);
  }
  public CursorIndexOutOfBoundsException(String message) { super(message); }
}
