package android.database;

import android.content.ContentResolver;
import android.net.Uri;
import android.os.Bundle;
import java.util.ArrayList;
import java.util.Arrays;

/** AOSP android.database.AbstractCursor: position, column lookup and observer bookkeeping. */
public abstract class AbstractCursor implements Cursor {
  protected int mPos = -1;
  protected boolean mClosed;
  private final ArrayList<ContentObserver> contentObservers = new ArrayList<>();
  private final ArrayList<DataSetObserver> dataSetObservers = new ArrayList<>();
  private Uri notifyUri;
  private Bundle extras = Bundle.EMPTY;

  public abstract int getCount();
  public abstract String[] getColumnNames();
  public abstract String getString(int column);
  public abstract short getShort(int column);
  public abstract int getInt(int column);
  public abstract long getLong(int column);
  public abstract float getFloat(int column);
  public abstract double getDouble(int column);
  public abstract boolean isNull(int column);
  public int getType(int column) { return FIELD_TYPE_STRING; }
  public byte[] getBlob(int column) { throw new UnsupportedOperationException("getBlob is not supported"); }
  /** Windowed cursors return their window; other cursors have none. */
  public CursorWindow getWindow() { return null; }
  public int getColumnCount() { return getColumnNames().length; }
  @Deprecated public void deactivate() { onDeactivateOrClose(); }
  @Deprecated public boolean requery() {
    for (var o : new ArrayList<>(dataSetObservers)) o.onChanged();
    return true;
  }
  protected void onDeactivateOrClose() {
    for (var o : new ArrayList<>(dataSetObservers)) o.onInvalidated();
  }
  public boolean isClosed() { return mClosed; }
  public void close() {
    mClosed = true;
    contentObservers.clear();
    onDeactivateOrClose();
  }
  public boolean onMove(int oldPosition, int newPosition) { return true; }
  public final int getPosition() { return mPos; }
  public final boolean moveToPosition(int position) {
    final int count = getCount();
    if (position >= count) {
      mPos = count;
      return false;
    }
    if (position < 0) {
      mPos = -1;
      return false;
    }
    if (position == mPos) return true;
    boolean result = onMove(mPos, position);
    mPos = result ? position : -1;
    return result;
  }
  public final boolean move(int offset) { return moveToPosition(mPos + offset); }
  public final boolean moveToFirst() { return moveToPosition(0); }
  public final boolean moveToLast() { return moveToPosition(getCount() - 1); }
  public final boolean moveToNext() { return moveToPosition(mPos + 1); }
  public final boolean moveToPrevious() { return moveToPosition(mPos - 1); }
  public final boolean isFirst() { return mPos == 0 && getCount() != 0; }
  public final boolean isLast() { int count = getCount(); return mPos == count - 1 && count != 0; }
  public final boolean isBeforeFirst() { return getCount() == 0 || mPos == -1; }
  public final boolean isAfterLast() { return getCount() == 0 || mPos == getCount(); }
  public int getColumnIndex(String columnName) {
    int dot = columnName.lastIndexOf('.');  // "table.column" names the column
    if (dot != -1) columnName = columnName.substring(dot + 1);
    String[] names = getColumnNames();
    for (int i = 0; i < names.length; ++i)
      if (names[i].equalsIgnoreCase(columnName)) return i;
    return -1;
  }
  public int getColumnIndexOrThrow(String columnName) {
    int index = getColumnIndex(columnName);
    if (index < 0)
      throw new IllegalArgumentException("column '" + columnName + "' does not exist. Available columns: "
          + Arrays.toString(getColumnNames()));
    return index;
  }
  public String getColumnName(int columnIndex) { return getColumnNames()[columnIndex]; }
  public void registerContentObserver(ContentObserver observer) { contentObservers.add(observer); }
  public void unregisterContentObserver(ContentObserver observer) {
    if (!mClosed) contentObservers.remove(observer);
  }
  public void registerDataSetObserver(DataSetObserver observer) { dataSetObservers.add(observer); }
  public void unregisterDataSetObserver(DataSetObserver observer) { dataSetObservers.remove(observer); }
  public void setNotificationUri(ContentResolver cr, Uri uri) { notifyUri = uri; }
  public Uri getNotificationUri() { return notifyUri; }
  public boolean getWantsAllOnMoveCalls() { return false; }
  public void setExtras(Bundle value) { extras = value == null ? Bundle.EMPTY : value; }
  public Bundle getExtras() { return extras; }
  public Bundle respond(Bundle value) { return Bundle.EMPTY; }
  protected void checkPosition() {
    if (mPos == -1 || getCount() == mPos) throw new CursorIndexOutOfBoundsException(mPos, getCount());
  }
}
