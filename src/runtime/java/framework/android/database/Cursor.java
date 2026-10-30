package android.database;

import android.content.ContentResolver;
import android.net.Uri;
import android.os.Bundle;
import java.io.Closeable;
import java.util.List;

/** AOSP android.database.Cursor: random read access to a query's result rows. */
public interface Cursor extends Closeable {
  int FIELD_TYPE_NULL = 0, FIELD_TYPE_INTEGER = 1, FIELD_TYPE_FLOAT = 2, FIELD_TYPE_STRING = 3,
      FIELD_TYPE_BLOB = 4;
  int getCount();
  int getPosition();
  boolean move(int offset);
  boolean moveToPosition(int position);
  boolean moveToFirst();
  boolean moveToLast();
  boolean moveToNext();
  boolean moveToPrevious();
  boolean isFirst();
  boolean isLast();
  boolean isBeforeFirst();
  boolean isAfterLast();
  int getColumnIndex(String columnName);
  int getColumnIndexOrThrow(String columnName) throws IllegalArgumentException;
  String getColumnName(int columnIndex);
  String[] getColumnNames();
  int getColumnCount();
  byte[] getBlob(int columnIndex);
  String getString(int columnIndex);
  short getShort(int columnIndex);
  int getInt(int columnIndex);
  long getLong(int columnIndex);
  float getFloat(int columnIndex);
  double getDouble(int columnIndex);
  int getType(int columnIndex);
  boolean isNull(int columnIndex);
  @Deprecated void deactivate();
  @Deprecated boolean requery();
  void close();
  boolean isClosed();
  void registerContentObserver(ContentObserver observer);
  void unregisterContentObserver(ContentObserver observer);
  void registerDataSetObserver(DataSetObserver observer);
  void unregisterDataSetObserver(DataSetObserver observer);
  void setNotificationUri(ContentResolver cr, Uri uri);
  Uri getNotificationUri();
  default void setNotificationUris(ContentResolver cr, List<Uri> uris) { setNotificationUri(cr, uris.get(0)); }
  default List<Uri> getNotificationUris() {
    Uri uri = getNotificationUri();
    return uri == null ? null : List.of(uri);
  }
  boolean getWantsAllOnMoveCalls();
  void setExtras(Bundle extras);
  Bundle getExtras();
  Bundle respond(Bundle extras);
}
