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
  /** The first column of the first row as a long; SQLiteDoneException when there is no row. */
  public long simpleQueryForLong() {
    Object value = simpleQuery();
    if (value instanceof Number n) return n.longValue();
    if (value == null) return 0;
    try { return Long.parseLong(value.toString().trim()); } catch (NumberFormatException e) { return 0; }
  }
  public String simpleQueryForString() {
    Object value = simpleQuery();
    if (value instanceof byte[] b) return new String(b, java.nio.charset.StandardCharsets.UTF_8);
    return value != null ? value.toString() : null;
  }
  private Object simpleQuery() {
    var rows = new java.util.ArrayList<Object[]>();
    getDatabase().query(getSql(), getBindArgs(), rows);
    if (rows.isEmpty()) throw new SQLiteDoneException();
    Object[] row = rows.get(0);
    return row.length > 0 ? row[0] : null;
  }
  @Override public String toString() { return "SQLiteProgram: " + getSql(); }
}
