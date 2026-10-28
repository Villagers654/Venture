package android.database.sqlite;

/** AOSP: a corrupt database file (SQLITE_CORRUPT, SQLITE_NOTADB). */
public class SQLiteDatabaseCorruptException extends SQLiteException {
  public SQLiteDatabaseCorruptException() {}
  public SQLiteDatabaseCorruptException(String error) { super(error); }
}
