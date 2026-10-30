package android.database.sqlite;

/** AOSP android.database.sqlite.SQLiteDoneException: a simple query returned no rows. */
public class SQLiteDoneException extends SQLiteException {
  public SQLiteDoneException() {}
  public SQLiteDoneException(String error) { super(error); }
}
