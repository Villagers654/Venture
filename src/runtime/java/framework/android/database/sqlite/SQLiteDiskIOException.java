package android.database.sqlite;

/** AOSP: a disk I/O failure (SQLITE_IOERR). */
public class SQLiteDiskIOException extends SQLiteException {
  public SQLiteDiskIOException() {}
  public SQLiteDiskIOException(String error) { super(error); }
}
