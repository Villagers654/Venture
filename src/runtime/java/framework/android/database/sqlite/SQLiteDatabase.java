package android.database.sqlite;

import android.database.Cursor;
import java.io.Closeable;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.locks.ReentrantLock;

/**
 * AOSP android.database.sqlite.SQLiteDatabase over the host's system SQLite.
 * Like an AOSP database without write-ahead logging, it has one connection: a
 * thread holds it for each call and for the whole of a transaction. BEGIN,
 * COMMIT/END and ROLLBACK statements drive the same transaction stack as
 * beginTransaction()/endTransaction() (SQLiteSession.executeSpecial). The
 * LOCALIZED and UNICODE collations are not registered.
 */
public final class SQLiteDatabase implements Closeable {
  public static final int OPEN_READWRITE = 0x00000000;
  public static final int OPEN_READONLY = 0x00000001;
  public static final int NO_LOCALIZED_COLLATORS = 0x00000010;
  public static final int CREATE_IF_NECESSARY = 0x10000000;
  public static final String MEMORY_DB_PATH = ":memory:";

  public interface CursorFactory {
    Cursor newCursor(SQLiteDatabase db, SQLiteCursorDriver masterQuery, String editTable, SQLiteQuery query);
  }

  private static final class Transaction {
    final Transaction parent;
    boolean marked, childFailed;
    Transaction(Transaction parent) { this.parent = parent; }
  }

  private final String path;
  private final int flags;
  private final CursorFactory factory;
  private final ReentrantLock connection = new ReentrantLock();
  private long handle;
  private Transaction transaction;

  private SQLiteDatabase(String path, int flags, CursorFactory factory) {
    this.path = path;
    this.flags = flags;
    this.factory = factory;
  }

  public static SQLiteDatabase openDatabase(String path, CursorFactory factory, int flags) {
    var db = new SQLiteDatabase(path, flags, factory);
    // SQLiteConnection.nativeOpen: SQLite accepts READONLY, READWRITE or
    // READWRITE|CREATE, and CREATE_IF_NECESSARY takes precedence.
    int sqliteFlags = (flags & CREATE_IF_NECESSARY) != 0 ? SQLiteNative.OPEN_READWRITE | SQLiteNative.OPEN_CREATE
        : (flags & OPEN_READONLY) != 0 ? SQLiteNative.OPEN_READONLY : SQLiteNative.OPEN_READWRITE;
    byte[] utf8 = path.getBytes(StandardCharsets.UTF_8);
    db.handle = SQLiteNative.open(Arrays.copyOf(utf8, utf8.length + 1), sqliteFlags);
    return db;
  }
  public static SQLiteDatabase openOrCreateDatabase(File file, CursorFactory factory) {
    return openOrCreateDatabase(file.getPath(), factory);
  }
  public static SQLiteDatabase openOrCreateDatabase(String path, CursorFactory factory) {
    return openDatabase(path, factory, CREATE_IF_NECESSARY);
  }
  /** An in-memory database, deleted when closed. */
  public static SQLiteDatabase create(CursorFactory factory) {
    return openDatabase(MEMORY_DB_PATH, factory, CREATE_IF_NECESSARY);
  }

  public String getPath() { return path; }
  public boolean isReadOnly() { return (flags & OPEN_READONLY) != 0; }
  public boolean isOpen() {
    connection.lock();
    try { return handle != 0; } finally { connection.unlock(); }
  }
  @Override public void close() {
    connection.lock();
    try {
      if (handle != 0) SQLiteNative.close(handle);
      handle = 0;
    } finally { connection.unlock(); }
  }

  public void execSQL(String sql) { execSQL(sql, null); }
  public void execSQL(String sql, Object[] bindArgs) {
    Object[] values = null;
    if (bindArgs != null) {
      values = new Object[bindArgs.length];
      for (int i = 0; i < bindArgs.length; ++i) values[i] = bindValue(bindArgs[i]);
    }
    try (var statement = new SQLiteStatement(this, sql, values)) {
      statement.executeUpdateDelete();
    }
  }
  public SQLiteStatement compileStatement(String sql) { return new SQLiteStatement(this, sql, null); }
  public Cursor rawQuery(String sql, String[] selectionArgs) { return rawQueryWithFactory(null, sql, selectionArgs, null); }
  public Cursor rawQueryWithFactory(CursorFactory cursorFactory, String sql, String[] selectionArgs, String editTable) {
    var driver = new SQLiteDirectCursorDriver(this, sql, editTable);
    return driver.query(cursorFactory != null ? cursorFactory : factory, selectionArgs);
  }

  public void beginTransaction() { begin("BEGIN EXCLUSIVE;"); }
  public void beginTransactionNonExclusive() { begin("BEGIN IMMEDIATE;"); }
  public void setTransactionSuccessful() {
    requireTransaction();
    if (transaction.marked)
      throw new IllegalStateException("Cannot perform this operation because the transaction has already been "
          + "marked successful.  The only thing you can do now is call endTransaction().");
    transaction.marked = true;
  }
  public void endTransaction() {
    requireTransaction();
    try {
      var top = transaction;
      boolean successful = top.marked && !top.childFailed;
      transaction = top.parent;
      if (transaction != null) {
        if (!successful) transaction.childFailed = true;
      } else {
        SQLiteNative.execute(open(), successful ? "COMMIT;" : "ROLLBACK;", null, SQLiteNative.RESULT_NONE);
      }
    } finally {
      connection.unlock();  // releases the hold taken by begin()
    }
  }
  public boolean inTransaction() { return connection.isHeldByCurrentThread() && transaction != null; }
  @Override public String toString() { return "SQLiteDatabase: " + path; }

  Object[] describe(String sql) {
    connection.lock();
    try { return SQLiteNative.prepare(open(), sql); } finally { connection.unlock(); }
  }
  long execute(String sql, Object[] args, int result) {
    if (executeSpecial(sql)) return 0;
    connection.lock();
    try { return SQLiteNative.execute(open(), sql, args, result); } finally { connection.unlock(); }
  }
  void query(String sql, Object[] args, List<Object[]> rows) {
    if (executeSpecial(sql)) return;
    connection.lock();
    try { SQLiteNative.query(open(), sql, args, rows); } finally { connection.unlock(); }
  }

  static boolean isTransactionStatement(String sql) { return transactionVerb(sql) != null; }
  // DatabaseUtils.getSqlStatementType: the first three letters name the statement.
  // Since API 28 a rollback containing " TO " (to a savepoint) is STATEMENT_OTHER.
  private static String transactionVerb(String sql) {
    String trimmed = sql.trim();
    if (trimmed.length() < 3) return null;
    return switch (trimmed.substring(0, 3).toUpperCase(Locale.ROOT)) {
      case "BEG" -> "BEGIN";
      case "COM", "END" -> "COMMIT";
      case "ROL" -> trimmed.toUpperCase(Locale.ROOT).contains(" TO ") ? null : "ROLLBACK";
      default -> null;
    };
  }
  private boolean executeSpecial(String sql) {
    String verb = transactionVerb(sql);
    if (verb == null) return false;
    switch (verb) {
      case "BEGIN" -> beginTransaction();
      case "COMMIT" -> { setTransactionSuccessful(); endTransaction(); }
      default -> endTransaction();
    }
    return true;
  }
  private void begin(String sql) {
    connection.lock();  // held until the matching endTransaction()
    try {
      if (transaction == null) SQLiteNative.execute(open(), sql, null, SQLiteNative.RESULT_NONE);
      transaction = new Transaction(transaction);
    } catch (RuntimeException e) {
      connection.unlock();
      throw e;
    }
  }
  private void requireTransaction() {
    if (!inTransaction())
      throw new IllegalStateException("Cannot perform this operation because there is no current transaction.");
  }
  private long open() {
    if (handle == 0)
      throw new IllegalStateException("attempt to re-open an already-closed object: " + this);
    return handle;
  }
  // SQLiteConnection.bindArguments via DatabaseUtils.getTypeOfObject.
  private static Object bindValue(Object value) {
    if (value == null || value instanceof byte[] || value instanceof String || value instanceof Long
        || value instanceof Double) return value;
    if (value instanceof Float f) return (double) f;
    if (value instanceof Integer || value instanceof Short || value instanceof Byte) return ((Number) value).longValue();
    if (value instanceof Boolean b) return b ? 1L : 0L;
    return value.toString();
  }
}
