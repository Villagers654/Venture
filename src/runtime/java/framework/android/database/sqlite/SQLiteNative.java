package android.database.sqlite;

import java.util.List;

/**
 * The host's system SQLite (sqlite_bridge.cpp), standing in for AOSP's
 * android_database_SQLiteConnection.cpp. Each call compiles the first statement
 * of {@code sql}, as AOSP does, and finalizes it before returning.
 */
final class SQLiteNative {
  static final int OPEN_READONLY = 0x1, OPEN_READWRITE = 0x2, OPEN_CREATE = 0x4;
  static final int RESULT_NONE = 0, RESULT_CHANGES = 1, RESULT_ROWID = 2;
  private static final String[] NAMES = {"SQLITE_OK", "SQLITE_ERROR", "SQLITE_INTERNAL", "SQLITE_PERM",
      "SQLITE_ABORT", "SQLITE_BUSY", "SQLITE_LOCKED", "SQLITE_NOMEM", "SQLITE_READONLY", "SQLITE_INTERRUPT",
      "SQLITE_IOERR", "SQLITE_CORRUPT", "SQLITE_NOTFOUND", "SQLITE_FULL", "SQLITE_CANTOPEN", "SQLITE_PROTOCOL",
      "SQLITE_EMPTY", "SQLITE_SCHEMA", "SQLITE_TOOBIG", "SQLITE_CONSTRAINT", "SQLITE_MISMATCH", "SQLITE_MISUSE",
      "SQLITE_NOLFS", "SQLITE_AUTH", "SQLITE_FORMAT", "SQLITE_RANGE", "SQLITE_NOTADB"};

  private SQLiteNative() {}

  /** Opens {@code path} (NUL-terminated UTF-8) with sqlite3_open_v2 flags. */
  static native long open(byte[] path, int flags);
  static native void close(long db);
  /** Returns {Integer parameter count, String[] column names}. */
  static native Object[] prepare(long db, String sql);
  /** Runs a statement that returns no rows; the result is per RESULT_*. */
  static native long execute(long db, String sql, Object[] args, int result);
  /** Appends each result row (Long, Double, String, byte[] or null values). */
  static native void query(long db, String sql, Object[] args, List<Object[]> rows);

  /** AOSP throw_sqlite3_exception: the exception class follows the primary result code. */
  static SQLiteException exception(int code, String message, String detail) {
    int primary = code & 0xff;
    String text = message + " (code " + code + (primary < NAMES.length ? " " + NAMES[primary] : "") + ")"
        + (detail == null ? "" : ": " + detail);
    return switch (primary) {
      case 5 -> new SQLiteDatabaseLockedException(text);
      case 10 -> new SQLiteDiskIOException(text);
      case 11, 26 -> new SQLiteDatabaseCorruptException(text);
      case 13 -> new SQLiteFullException(text);
      case 14 -> new SQLiteCantOpenDatabaseException(text);
      case 19 -> new SQLiteConstraintException(text);
      default -> new SQLiteException(text);
    };
  }
}
