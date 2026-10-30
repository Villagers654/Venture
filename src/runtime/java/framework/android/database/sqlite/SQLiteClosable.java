package android.database.sqlite;

import java.io.Closeable;

/** AOSP android.database.sqlite.SQLiteClosable. Databases and programs here are closed directly, without reference counts. */
public abstract class SQLiteClosable implements Closeable {
  public void acquireReference() {}
  public void releaseReference() {}
  @Override public abstract void close();
}
