package android.database.sqlite;

/** AOSP: an access permission was denied (SQLITE_PERM). */
public class SQLiteAccessPermException extends SQLiteException {
  public SQLiteAccessPermException() {}
  public SQLiteAccessPermException(String error) { super(error); }
}
