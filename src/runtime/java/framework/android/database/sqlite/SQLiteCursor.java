package android.database.sqlite;

import android.database.AbstractWindowedCursor;
import android.database.CursorWindow;
import java.util.HashMap;
import java.util.Map;

/** AOSP android.database.sqlite.SQLiteCursor: query rows, run on first use and kept in a window. */
public class SQLiteCursor extends AbstractWindowedCursor {
  private static final int NO_COUNT = -1;
  private final String editTable;
  private final String[] columns;
  private final SQLiteQuery query;
  private final SQLiteCursorDriver driver;
  private int count = NO_COUNT;
  private Map<String, Integer> columnNameMap;

  @Deprecated
  public SQLiteCursor(SQLiteDatabase db, SQLiteCursorDriver driver, String editTable, SQLiteQuery query) {
    this(driver, editTable, query);
  }
  public SQLiteCursor(SQLiteCursorDriver driver, String editTable, SQLiteQuery query) {
    if (query == null) throw new IllegalArgumentException("query object cannot be null");
    this.driver = driver;
    this.editTable = editTable;
    this.query = query;
    this.columns = query.getColumnNames();
  }
  public SQLiteDatabase getDatabase() { return query.getDatabase(); }
  @Override public boolean onMove(int oldPosition, int newPosition) {
    if (mWindow == null || newPosition < mWindow.getStartPosition()
        || newPosition >= mWindow.getStartPosition() + mWindow.getNumRows())
      fillWindow();
    return true;
  }
  @Override public int getCount() {
    if (count == NO_COUNT) fillWindow();
    return count;
  }
  private void fillWindow() {
    clearOrCreateWindow(getDatabase().getPath());
    count = query.fillWindow(mWindow);
  }
  /** Case-sensitive, unlike AbstractCursor, as in AOSP. */
  @Override public int getColumnIndex(String columnName) {
    if (columnNameMap == null) {
      var map = new HashMap<String, Integer>(columns.length);
      for (int i = 0; i < columns.length; ++i) map.put(columns[i], i);
      columnNameMap = map;
    }
    int period = columnName.lastIndexOf('.');
    if (period != -1) columnName = columnName.substring(period + 1);
    Integer index = columnNameMap.get(columnName);
    return index != null ? index : -1;
  }
  @Override public String[] getColumnNames() { return columns; }
  @Override @Deprecated public void deactivate() {
    super.deactivate();
    driver.cursorDeactivated();
  }
  @Override public void close() {
    super.close();
    synchronized (this) {
      query.close();
      driver.cursorClosed();
    }
  }
  @Override @Deprecated public boolean requery() {
    if (isClosed()) return false;
    synchronized (this) {
      if (!getDatabase().isOpen()) return false;
      if (mWindow != null) mWindow.clear();
      mPos = -1;
      count = NO_COUNT;
      driver.cursorRequeried(this);
    }
    try {
      return super.requery();
    } catch (IllegalStateException e) {
      return false;
    }
  }
  @Override public void setWindow(CursorWindow window) {
    super.setWindow(window);
    count = NO_COUNT;
  }
  public void setSelectionArguments(String[] selectionArgs) { driver.setBindArguments(selectionArgs); }
  public void setFillWindowForwardOnly(boolean fillWindowForwardOnly) {}
}
