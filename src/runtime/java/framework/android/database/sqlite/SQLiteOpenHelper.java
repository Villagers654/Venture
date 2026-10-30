package android.database.sqlite;

import android.content.Context;
import android.database.DatabaseErrorHandler;
import java.io.File;

/**
 * AOSP android.database.sqlite.SQLiteOpenHelper: opens the database on first use and creates,
 * upgrades or downgrades it to the requested version (PRAGMA user_version) in one transaction.
 */
public abstract class SQLiteOpenHelper implements AutoCloseable {
  private final Context context;
  private final String name;
  private final SQLiteDatabase.CursorFactory factory;
  private final int newVersion;
  private SQLiteDatabase database;
  private boolean initializing, writeAheadLogging;

  public SQLiteOpenHelper(Context context, String name, SQLiteDatabase.CursorFactory factory, int version) {
    this(context, name, factory, version, null);
  }
  public SQLiteOpenHelper(Context context, String name, SQLiteDatabase.CursorFactory factory, int version,
                          DatabaseErrorHandler errorHandler) {
    if (version < 1) throw new IllegalArgumentException("Version must be >= 1, was " + version);
    this.context = context;
    this.name = name;
    this.factory = factory;
    this.newVersion = version;
  }

  public String getDatabaseName() { return name; }
  public synchronized void setWriteAheadLoggingEnabled(boolean enabled) {
    if (writeAheadLogging == enabled) return;
    if (database != null && database.isOpen() && !database.isReadOnly()) {
      if (enabled) database.enableWriteAheadLogging();
      else database.disableWriteAheadLogging();
    }
    writeAheadLogging = enabled;
  }
  public SQLiteDatabase getWritableDatabase() {
    synchronized (this) { return getDatabaseLocked(true); }
  }
  public SQLiteDatabase getReadableDatabase() {
    synchronized (this) { return getDatabaseLocked(false); }
  }
  private SQLiteDatabase getDatabaseLocked(boolean writable) {
    if (database != null) {
      if (!database.isOpen()) database = null;
      else if (!writable || !database.isReadOnly()) return database;
    }
    if (initializing) throw new IllegalStateException("getDatabase called recursively");
    if (database != null) {  // read-only: reopened read-write below
      database.close();
      database = null;
    }
    SQLiteDatabase db = null;
    try {
      initializing = true;
      if (name == null) {
        db = SQLiteDatabase.create(factory);
      } else {
        File path = context.getDatabasePath(name);
        File parent = path.getParentFile();
        if (parent != null) parent.mkdirs();
        try {
          db = SQLiteDatabase.openDatabase(path.getPath(), factory, SQLiteDatabase.CREATE_IF_NECESSARY);
        } catch (SQLiteException e) {
          if (writable) throw e;
          db = SQLiteDatabase.openDatabase(path.getPath(), factory, SQLiteDatabase.OPEN_READONLY);
        }
      }
      if (writeAheadLogging && !db.isReadOnly()) db.enableWriteAheadLogging();
      onConfigure(db);
      int version = db.getVersion();
      if (version != newVersion) {
        if (db.isReadOnly())
          throw new SQLiteException("Can't upgrade read-only database from version " + version + " to "
              + newVersion + ": " + db.getPath());
        db.beginTransaction();
        try {
          if (version == 0) onCreate(db);
          else if (version > newVersion) onDowngrade(db, version, newVersion);
          else onUpgrade(db, version, newVersion);
          db.setVersion(newVersion);
          db.setTransactionSuccessful();
        } finally {
          db.endTransaction();
        }
      }
      onOpen(db);
      database = db;
      return db;
    } finally {
      initializing = false;
      if (db != null && db != database) db.close();
    }
  }
  public synchronized void close() {
    if (initializing) throw new IllegalStateException("Closed during initialization");
    if (database != null && database.isOpen()) database.close();
    database = null;
  }

  public void onConfigure(SQLiteDatabase db) {}
  public abstract void onCreate(SQLiteDatabase db);
  public abstract void onUpgrade(SQLiteDatabase db, int oldVersion, int newVersion);
  public void onDowngrade(SQLiteDatabase db, int oldVersion, int newVersion) {
    throw new SQLiteException("Can't downgrade database from version " + oldVersion + " to " + newVersion);
  }
  public void onOpen(SQLiteDatabase db) {}
}
