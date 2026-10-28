package android.database;

/** AOSP android.database.SQLException: an error executing SQL. */
public class SQLException extends RuntimeException {
  public SQLException() {}
  public SQLException(String error) { super(error); }
  public SQLException(String error, Throwable cause) { super(error, cause); }
}
