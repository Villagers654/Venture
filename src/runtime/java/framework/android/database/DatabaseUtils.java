package android.database;

import android.database.sqlite.SQLiteDatabase;
import android.text.TextUtils;

/** The AOSP android.database.DatabaseUtils helpers apps call directly. */
public class DatabaseUtils {
  public static void appendEscapedSQLString(StringBuilder sb, String value) {
    sb.append('\'');
    for (int i = 0; i < value.length(); i++) {
      char c = value.charAt(i);
      if (c == '\'') sb.append('\'');
      sb.append(c);
    }
    sb.append('\'');
  }
  public static String sqlEscapeString(String value) {
    var sb = new StringBuilder();
    appendEscapedSQLString(sb, value);
    return sb.toString();
  }
  public static void appendValueToSql(StringBuilder sql, Object value) {
    if (value == null) sql.append("NULL");
    else if (value instanceof Boolean b) sql.append(b ? '1' : '0');
    else appendEscapedSQLString(sql, value.toString());
  }
  public static String concatenateWhere(String a, String b) {
    if (TextUtils.isEmpty(a)) return b;
    if (TextUtils.isEmpty(b)) return a;
    return "(" + a + ") AND (" + b + ")";
  }
  public static String[] appendSelectionArgs(String[] original, String[] added) {
    if (original == null || original.length == 0) return added;
    String[] result = new String[original.length + added.length];
    System.arraycopy(original, 0, result, 0, original.length);
    System.arraycopy(added, 0, result, original.length, added.length);
    return result;
  }
  public static long longForQuery(SQLiteDatabase db, String query, String[] selectionArgs) {
    try (var statement = db.compileStatement(query)) {
      if (selectionArgs != null) statement.bindAllArgsAsStrings(selectionArgs);
      return statement.simpleQueryForLong();
    }
  }
  public static String stringForQuery(SQLiteDatabase db, String query, String[] selectionArgs) {
    try (var statement = db.compileStatement(query)) {
      if (selectionArgs != null) statement.bindAllArgsAsStrings(selectionArgs);
      return statement.simpleQueryForString();
    }
  }
  public static long queryNumEntries(SQLiteDatabase db, String table) { return queryNumEntries(db, table, null, null); }
  public static long queryNumEntries(SQLiteDatabase db, String table, String selection) {
    return queryNumEntries(db, table, selection, null);
  }
  public static long queryNumEntries(SQLiteDatabase db, String table, String selection, String[] selectionArgs) {
    String where = !TextUtils.isEmpty(selection) ? " where " + selection : "";
    return longForQuery(db, "select count(*) from " + table + where, selectionArgs);
  }
}
