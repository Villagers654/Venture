package android.database.sqlite;

import java.util.Arrays;

/** AOSP android.database.sqlite.SQLiteProgram: a compiled statement's SQL, columns and bind arguments. */
public abstract class SQLiteProgram extends SQLiteClosable {
  private static final String[] EMPTY = new String[0];
  private final SQLiteDatabase database;
  private final String sql;
  private final String[] columnNames;
  private final int numParameters;
  private final Object[] bindArgs;

  SQLiteProgram(SQLiteDatabase db, String sql, Object[] bindArgs) {
    this.database = db;
    this.sql = sql.trim();
    if (SQLiteDatabase.isTransactionStatement(this.sql)) {
      columnNames = EMPTY;
      numParameters = 0;
    } else {
      Object[] info = db.describe(this.sql);  // compiles now so SQL errors surface here, as on Android
      numParameters = (Integer) info[0];
      columnNames = (String[]) info[1];
    }
    if (bindArgs != null && bindArgs.length > numParameters)
      throw new IllegalArgumentException("Too many bind arguments.  " + bindArgs.length
          + " arguments were provided but the statement needs " + numParameters + " arguments.");
    this.bindArgs = new Object[numParameters];
    if (bindArgs != null) System.arraycopy(bindArgs, 0, this.bindArgs, 0, bindArgs.length);
  }
  final SQLiteDatabase getDatabase() { return database; }
  final String getSql() { return sql; }
  final Object[] getBindArgs() { return bindArgs; }
  final String[] getColumnNames() { return columnNames; }
  @Deprecated public final int getUniqueId() { return -1; }
  public void bindNull(int index) { bind(index, null); }
  public void bindLong(int index, long value) { bind(index, value); }
  public void bindDouble(int index, double value) { bind(index, value); }
  public void bindString(int index, String value) {
    if (value == null) throw new IllegalArgumentException("the bind value at index " + index + " is null");
    bind(index, value);
  }
  public void bindBlob(int index, byte[] value) {
    if (value == null) throw new IllegalArgumentException("the bind value at index " + index + " is null");
    bind(index, value);
  }
  public void clearBindings() { Arrays.fill(bindArgs, null); }
  public void bindAllArgsAsStrings(String[] bindArgs) {
    if (bindArgs != null)
      for (int i = bindArgs.length; i != 0; i--) bindString(i, bindArgs[i - 1]);
  }
  private void bind(int index, Object value) {
    if (index < 1 || index > numParameters)
      throw new IllegalArgumentException("Cannot bind argument at index " + index
          + " because the index is out of range.  The statement has " + numParameters + " parameters.");
    bindArgs[index - 1] = value;
  }
  /** Statements are compiled per execution, so there is nothing to release. */
  @Override public void close() {}
}
