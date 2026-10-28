package android.database.sqlite;

/** AOSP: a full database or disk (SQLITE_FULL). */
public class SQLiteFullException extends SQLiteException {
  public SQLiteFullException() {}
  public SQLiteFullException(String error) { super(error); }
}
