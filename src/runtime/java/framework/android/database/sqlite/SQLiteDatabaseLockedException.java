package android.database.sqlite;

/** AOSP: a database locked by another connection (SQLITE_BUSY). */
public class SQLiteDatabaseLockedException extends SQLiteException {
  public SQLiteDatabaseLockedException() {}
  public SQLiteDatabaseLockedException(String error) { super(error); }
}
