package android.database.sqlite;

/** AOSP: a database that cannot be opened (SQLITE_CANTOPEN). */
public class SQLiteCantOpenDatabaseException extends SQLiteException {
  public SQLiteCantOpenDatabaseException() {}
  public SQLiteCantOpenDatabaseException(String error) { super(error); }
}
