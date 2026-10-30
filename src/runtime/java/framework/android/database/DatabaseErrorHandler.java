package android.database;

import android.database.sqlite.SQLiteDatabase;

/** AOSP android.database.DatabaseErrorHandler. */
public interface DatabaseErrorHandler {
  void onCorruption(SQLiteDatabase dbObj);
}
