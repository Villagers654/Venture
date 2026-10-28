package android.database.sqlite;

import android.database.Cursor;

/** AOSP android.database.sqlite.SQLiteDirectCursorDriver: the cursor driver of rawQuery. */
public final class SQLiteDirectCursorDriver implements SQLiteCursorDriver {
  private final SQLiteDatabase database;
  private final String editTable;
  private final String sql;
  private SQLiteQuery query;

  public SQLiteDirectCursorDriver(SQLiteDatabase db, String sql, String editTable) {
    this.database = db;
    this.editTable = editTable;
    this.sql = sql;
  }
  @Override public Cursor query(SQLiteDatabase.CursorFactory factory, String[] selectionArgs) {
    var compiled = new SQLiteQuery(database, sql);
    Cursor cursor;
    try {
      compiled.bindAllArgsAsStrings(selectionArgs);
      cursor = factory == null ? new SQLiteCursor(this, editTable, compiled)
                               : factory.newCursor(database, this, editTable, compiled);
    } catch (RuntimeException e) {
      compiled.close();
      throw e;
    }
    query = compiled;
    return cursor;
  }
  @Override public void cursorClosed() {}
  @Override public void setBindArguments(String[] bindArgs) { query.bindAllArgsAsStrings(bindArgs); }
  @Override public void cursorDeactivated() {}
  @Override public void cursorRequeried(Cursor cursor) {}
  @Override public String toString() { return "SQLiteDirectCursorDriver: " + sql; }
}
