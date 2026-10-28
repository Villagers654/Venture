package android.database.sqlite;

import android.database.CursorWindow;
import java.util.ArrayList;

/** AOSP android.database.sqlite.SQLiteQuery: runs a query into a cursor window. */
public final class SQLiteQuery extends SQLiteProgram {
  SQLiteQuery(SQLiteDatabase db, String query) { super(db, query, null); }

  /** Fills {@code window} with every result row and returns the row count. */
  int fillWindow(CursorWindow window) {
    var rows = new ArrayList<Object[]>();
    getDatabase().query(getSql(), getBindArgs(), rows);
    window.clear();
    window.setStartPosition(0);
    int columns = getColumnNames().length;
    if (!rows.isEmpty()) columns = rows.get(0).length;
    window.setNumColumns(columns);
    for (int row = 0; row < rows.size(); ++row) {
      window.allocRow();
      Object[] values = rows.get(row);
      for (int column = 0; column < values.length; ++column) {
        Object value = values[column];
        if (value == null) window.putNull(row, column);
        else if (value instanceof Long l) window.putLong(l, row, column);
        else if (value instanceof Double d) window.putDouble(d, row, column);
        else if (value instanceof String s) window.putString(s, row, column);
        else window.putBlob((byte[]) value, row, column);
      }
    }
    return rows.size();
  }
  @Override public String toString() { return "SQLiteQuery: " + getSql(); }
}
