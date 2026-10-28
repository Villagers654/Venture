package android.database.sqlite;

/** AOSP android.database.sqlite.SQLiteStatement: a statement run for its effect rather than rows. */
public final class SQLiteStatement extends SQLiteProgram {
  SQLiteStatement(SQLiteDatabase db, String sql, Object[] bindArgs) { super(db, sql, bindArgs); }
  /** Fails for statements that return rows, as on Android. */
  public void execute() { getDatabase().execute(getSql(), getBindArgs(), SQLiteNative.RESULT_NONE); }
  public int executeUpdateDelete() {
    return (int) getDatabase().execute(getSql(), getBindArgs(), SQLiteNative.RESULT_CHANGES);
  }
  /** The new row id, or -1 when the statement inserted nothing. */
  public long executeInsert() { return getDatabase().execute(getSql(), getBindArgs(), SQLiteNative.RESULT_ROWID); }
  @Override public String toString() { return "SQLiteProgram: " + getSql(); }
}
