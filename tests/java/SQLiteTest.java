import android.database.Cursor;
import android.database.sqlite.*;
import java.io.File;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;

/** android.database.sqlite over the host's system SQLite; run by framework_sqlite_test.cpp through JNI. */
public final class SQLiteTest {
  private static void check(boolean ok, String message) {
    if (!ok) throw new AssertionError(message);
  }
  private interface Action { void run() throws Exception; }
  private static <T extends Throwable> T expect(Class<T> type, Action action, String message) {
    try {
      action.run();
    } catch (Throwable thrown) {
      if (type.isInstance(thrown)) return type.cast(thrown);
      throw new AssertionError(message + ": unexpected " + thrown, thrown);
    }
    throw new AssertionError(message + ": nothing thrown");
  }
  private static long count(SQLiteDatabase db, String table) {
    try (Cursor c = db.rawQuery("SELECT COUNT(*) FROM " + table, null)) {
      check(c.moveToNext(), "count row");
      return c.getLong(0);
    }
  }

  public static void run(String directory) throws Exception {
    var db = SQLiteDatabase.create(null);
    db.execSQL("CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, score REAL, data BLOB, note TEXT, v, w);");
    db.execSQL("INSERT INTO t(name, score, data, note) VALUES(?, ?, ?, ?)",
               new Object[]{"alpha", 1.5f, new byte[]{1, 2, 3}, null});
    db.execSQL("INSERT INTO t(name, v, w) VALUES(?, ?, ?)", new Object[]{"beta", 7, true});
    try (Cursor c = db.rawQuery("SELECT id, name, score, data, note FROM t WHERE name = ?", new String[]{"alpha"})) {
      check(c instanceof SQLiteCursor && c.getColumnCount() == 5 && c.getColumnName(2).equals("score"), "query columns");
      check(c.getCount() == 1 && c.moveToNext() && c.getPosition() == 0, "one matching row");
      var window = ((SQLiteCursor) c).getWindow();
      int[] types = new int[5];
      for (int i = 0; i < 5; ++i) types[i] = window.getType(c.getPosition(), i);
      check(Arrays.equals(types, new int[]{Cursor.FIELD_TYPE_INTEGER, Cursor.FIELD_TYPE_STRING, Cursor.FIELD_TYPE_FLOAT,
                                           Cursor.FIELD_TYPE_BLOB, Cursor.FIELD_TYPE_NULL}), "SQLite storage classes");
      check(c.getInt(0) == 1 && c.getString(1).equals("alpha") && c.getDouble(2) == 1.5 && c.getString(2).equals("1.5")
          && Arrays.equals(c.getBlob(3), new byte[]{1, 2, 3}) && c.isNull(4), "row values");
      check(c.getColumnIndex("t.name") == 1 && c.getColumnIndex("NAME") == -1, "SQLiteCursor column names are exact");
      check(!c.moveToNext() && c.isAfterLast(), "rows end");
    }
    try (Cursor c = db.rawQuery("SELECT v, w FROM t WHERE name = 'beta'", null)) {
      check(c.moveToNext() && c.getType(0) == Cursor.FIELD_TYPE_INTEGER && c.getLong(0) == 7
          && c.getType(1) == Cursor.FIELD_TYPE_INTEGER && c.getLong(1) == 1, "Integer and Boolean bind as integers");
    }

    // ContentValues helpers (ExoPlayer's cache index and version table).
    db.execSQL("CREATE TABLE kv(k TEXT PRIMARY KEY, v INTEGER, b BLOB)");
    var values = new android.content.ContentValues();
    values.put("k", "one"); values.put("v", 1); values.put("b", new byte[]{9});
    check(db.insert("kv", null, values) > 0, "insert returns the row id");
    expect(SQLiteConstraintException.class, () -> db.insertOrThrow("kv", null, values), "insertOrThrow throws");
    values.put("v", 2);
    check(db.replaceOrThrow("kv", null, values) > 0 && android.database.DatabaseUtils.queryNumEntries(db, "kv") == 1, "replace");
    var changed = new android.content.ContentValues();
    changed.put("v", 5);
    check(db.update("kv", changed, "k = ?", new String[]{"one"}) == 1, "update counts rows");
    try (Cursor c = db.query("kv", new String[]{"k", "v"}, "v > ?", new String[]{"4"}, null, null, "k")) {
      check(c.moveToNext() && c.getString(0).equals("one") && c.getLong(1) == 5 && !c.moveToNext(), "query builds SELECT");
    }
    check(android.database.DatabaseUtils.longForQuery(db, "SELECT v FROM kv WHERE k = ?", new String[]{"one"}) == 5, "longForQuery");
    expect(SQLiteDoneException.class, () -> android.database.DatabaseUtils.stringForQuery(db, "SELECT k FROM kv WHERE v = 0", null),
        "a simple query without rows throws SQLiteDoneException");
    check(db.delete("kv", "k = ?", new String[]{"one"}) == 1 && android.database.DatabaseUtils.queryNumEntries(db, "kv") == 0,
        "delete counts rows");

    // GameAnalytics runs DDL and writes through rawQuery; AOSP executes them on the first move.
    Cursor lazy = db.rawQuery("INSERT OR REPLACE INTO t(name) VALUES(?);", new String[]{"lazy"});
    check(count(db, "t") == 2, "rawQuery compiles without executing");
    check(!lazy.moveToNext() && lazy.getCount() == 0 && count(db, "t") == 3, "the first move runs the statement");
    lazy.close();

    var constraint = expect(SQLiteConstraintException.class,
        () -> db.execSQL("INSERT INTO t(name) VALUES('alpha')"), "unique names");
    check(constraint.getMessage().equals("UNIQUE constraint failed: t.name (code 2067 SQLITE_CONSTRAINT)"),
          "AOSP error text: " + constraint.getMessage());
    var compile = expect(SQLiteException.class, () -> db.rawQuery("SELECT * FROM missing", null), "missing table");
    check(compile.getMessage().equals("no such table: missing (code 1 SQLITE_ERROR): , while compiling: SELECT * FROM missing"),
          "compile errors name the SQL: " + compile.getMessage());
    var rows = expect(SQLiteException.class, () -> db.execSQL("SELECT 1"), "execSQL rows");
    check(rows.getMessage().endsWith("Queries can be performed using SQLiteDatabase query or rawQuery methods only."),
          "execSQL refuses queries");
    expect(IllegalArgumentException.class, () -> db.rawQuery("SELECT ?", new String[]{null}), "null selection args");
    expect(IllegalArgumentException.class, () -> db.execSQL("SELECT 1 WHERE 1 = ?", new Object[]{1, 2}), "extra args");

    db.execSQL("BEGIN;");
    check(db.inTransaction(), "BEGIN starts the session transaction");
    db.execSQL("DELETE FROM t");
    db.execSQL("ROLLBACK");
    check(!db.inTransaction() && count(db, "t") == 3, "ROLLBACK restores rows");
    db.beginTransaction();
    db.beginTransaction();
    db.execSQL("DELETE FROM t WHERE name = 'beta'");
    db.endTransaction();
    db.setTransactionSuccessful();
    db.endTransaction();
    check(count(db, "t") == 3, "an unsuccessful nested transaction rolls back the outer one");
    db.execSQL("BEGIN;");
    db.execSQL("DELETE FROM t WHERE name = 'lazy'");
    db.execSQL("COMMIT");
    check(count(db, "t") == 2, "COMMIT keeps changes");
    // DatabaseUtils (API 28+): a ROLLBACK ... TO savepoint is an ordinary statement.
    db.beginTransaction();
    db.execSQL("SAVEPOINT sp");
    db.execSQL("DELETE FROM t");
    db.execSQL("ROLLBACK TO sp");
    check(db.inTransaction() && count(db, "t") == 2, "ROLLBACK TO a savepoint keeps the session transaction");
    db.setTransactionSuccessful();
    db.endTransaction();
    check(!db.inTransaction() && count(db, "t") == 2, "the outer transaction still commits");
    expect(IllegalStateException.class, () -> db.execSQL("COMMIT"), "COMMIT without a transaction");

    // One connection: another thread waits while a transaction holds it.
    db.beginTransaction();
    var started = new CountDownLatch(1);
    var finished = new AtomicBoolean();
    var other = new Thread(() -> {
      started.countDown();
      count(db, "t");
      finished.set(true);
    });
    other.start();
    check(started.await(5, TimeUnit.SECONDS), "reader started");
    Thread.sleep(100);
    check(!finished.get(), "readers wait for the open transaction");
    db.setTransactionSuccessful();
    db.endTransaction();
    other.join(5000);
    check(finished.get(), "the reader proceeds after the transaction");

    var insert = db.compileStatement("INSERT INTO t(name) VALUES(?)");
    insert.bindString(1, "gamma");
    check(insert.executeInsert() == 3, "executeInsert returns the row id");
    var update = db.compileStatement("UPDATE t SET note = ? WHERE id > ?");
    update.bindString(1, "n");
    update.bindLong(2, 0);
    check(update.executeUpdateDelete() == 3, "executeUpdateDelete counts rows");
    db.close();
    check(!db.isOpen(), "closed");
    expect(IllegalStateException.class, () -> db.rawQuery("SELECT 1", null), "closed databases refuse work");

    var file = new File(directory, "ga ü.sqlite3");
    var disk = SQLiteDatabase.openOrCreateDatabase(file.getPath(), null);
    check(disk.getPath().equals(file.getPath()) && !disk.isReadOnly(), "file database path");
    disk.execSQL("CREATE TABLE IF NOT EXISTS ga_state(key CHAR(255) PRIMARY KEY NOT NULL, value TEXT);");
    disk.execSQL("INSERT OR REPLACE INTO ga_state (key, value) VALUES(?, ?);", new Object[]{"k", "ü😀"});
    disk.close();
    check(file.isFile() && file.length() > 0, "database file written");
    var readOnly = SQLiteDatabase.openDatabase(file.getPath(), null, SQLiteDatabase.OPEN_READONLY);
    try (Cursor c = readOnly.rawQuery("SELECT value FROM ga_state WHERE key = ?", new String[]{"k"})) {
      check(c.moveToNext() && c.getString(0).equals("ü😀"), "UTF-16 text round-trips through the file");
    }
    expect(SQLiteException.class, () -> readOnly.execSQL("DELETE FROM ga_state"), "read-only writes");
    readOnly.close();
    expect(SQLiteCantOpenDatabaseException.class,
        () -> SQLiteDatabase.openDatabase(new File(directory, "missing/x.db").getPath(), null, SQLiteDatabase.OPEN_READWRITE),
        "missing directories cannot open");
    // SQLiteConnection.nativeOpen: CREATE_IF_NECESSARY opens read-write and creates, even with OPEN_READONLY.
    var created = new File(directory, "readonly-create.db");
    var both = SQLiteDatabase.openDatabase(created.getPath(), null,
        SQLiteDatabase.OPEN_READONLY | SQLiteDatabase.CREATE_IF_NECESSARY);
    check(both.isReadOnly() && created.isFile(), "OPEN_READONLY | CREATE_IF_NECESSARY creates the database");
    both.close();
  }
}
