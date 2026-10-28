package android.database.sqlite;

/** AOSP: a constraint violation (SQLITE_CONSTRAINT). */
public class SQLiteConstraintException extends SQLiteException {
  public SQLiteConstraintException() {}
  public SQLiteConstraintException(String error) { super(error); }
}
