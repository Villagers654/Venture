package android.database.sqlite;

/** AOSP: a table in the database is locked (SQLITE_LOCKED). */
public class SQLiteTableLockedException extends SQLiteException {
  public SQLiteTableLockedException() {}
  public SQLiteTableLockedException(String error) { super(error); }
}
