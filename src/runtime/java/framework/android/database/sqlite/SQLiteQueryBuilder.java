package android.database.sqlite;

import android.database.Cursor;
import android.database.DatabaseUtils;
import android.os.CancellationSignal;
import java.util.Map;
import java.util.Set;

/** AOSP SQLiteQueryBuilder: builds SELECTs over the tables, the appended WHERE and a projection map. */
public class SQLiteQueryBuilder {
  private Map<String, String> projectionMap;
  private String tables = "";
  private StringBuilder where;
  private boolean distinct;
  private SQLiteDatabase.CursorFactory factory;
  private boolean strict;

  public SQLiteQueryBuilder() {}

  public void setDistinct(boolean distinct) { this.distinct = distinct; }
  public boolean isDistinct() { return distinct; }
  public String getTables() { return tables; }
  public void setTables(String inTables) { tables = inTables; }
  public void appendWhere(CharSequence inWhere) {
    if (where == null) where = new StringBuilder(inWhere.length() + 16);
    where.append(inWhere);
  }
  public void appendWhereEscapeString(String inWhere) {
    if (where == null) where = new StringBuilder(inWhere.length() + 16);
    DatabaseUtils.appendEscapedSQLString(where, inWhere);
  }
  public void appendWhereStandalone(CharSequence inWhere) {
    if (where == null) where = new StringBuilder(inWhere.length() + 16);
    if (where.length() > 0) where.append(" AND ");
    where.append('(').append(inWhere).append(')');
  }
  public void setProjectionMap(Map<String, String> columnMap) { projectionMap = columnMap; }
  public Map<String, String> getProjectionMap() { return projectionMap; }
  public void setCursorFactory(SQLiteDatabase.CursorFactory factory) { this.factory = factory; }
  public SQLiteDatabase.CursorFactory getCursorFactory() { return factory; }
  public void setStrict(boolean flag) { strict = flag; }
  public boolean isStrict() { return strict; }
  public void setStrictColumns(boolean strictColumns) {}
  public void setStrictGrammar(boolean strictGrammar) {}

  public static String buildQueryString(boolean distinct, String tables, String[] columns, String where,
                                        String groupBy, String having, String orderBy, String limit) {
    return SQLiteDatabase.buildQueryString(distinct, tables, columns, where, groupBy, having, orderBy, limit);
  }

  public static void appendColumns(StringBuilder s, String[] columns) {
    for (int i = 0; i < columns.length; i++) {
      if (columns[i] == null) continue;
      if (i > 0) s.append(", ");
      s.append(columns[i]);
    }
    s.append(' ');
  }

  public Cursor query(SQLiteDatabase db, String[] projectionIn, String selection, String[] selectionArgs,
                      String groupBy, String having, String sortOrder) {
    return query(db, projectionIn, selection, selectionArgs, groupBy, having, sortOrder, null, null);
  }
  public Cursor query(SQLiteDatabase db, String[] projectionIn, String selection, String[] selectionArgs,
                      String groupBy, String having, String sortOrder, String limit) {
    return query(db, projectionIn, selection, selectionArgs, groupBy, having, sortOrder, limit, null);
  }
  public Cursor query(SQLiteDatabase db, String[] projectionIn, String selection, String[] selectionArgs,
                      String groupBy, String having, String sortOrder, String limit,
                      CancellationSignal cancellationSignal) {
    if (tables == null) return null;
    String sql = buildQuery(projectionIn, selection, groupBy, having, sortOrder, limit);
    return db.rawQueryWithFactory(factory, sql, selectionArgs, SQLiteDatabase.findEditTable(tables),
                                  cancellationSignal);
  }

  public int update(SQLiteDatabase db, android.content.ContentValues values, String selection,
                    String[] selectionArgs) {
    return db.update(tables, values, combinedWhere(selection), selectionArgs);
  }
  public int delete(SQLiteDatabase db, String selection, String[] selectionArgs) {
    return db.delete(tables, combinedWhere(selection), selectionArgs);
  }
  public long insert(SQLiteDatabase db, android.content.ContentValues values) {
    return db.insert(tables, null, values);
  }

  private String combinedWhere(String selection) {
    boolean hasWhere = where != null && where.length() > 0;
    boolean hasSelection = selection != null && !selection.isEmpty();
    if (!hasWhere) return hasSelection ? selection : null;
    return hasSelection ? "(" + where + ") AND (" + selection + ")" : where.toString();
  }

  public String buildQuery(String[] projectionIn, String selection, String groupBy, String having,
                           String sortOrder, String limit) {
    String[] projection = computeProjection(projectionIn);
    String w = combinedWhere(selection);
    return buildQueryString(distinct, tables, projection, w, groupBy, having, sortOrder, limit);
  }
  @Deprecated
  public String buildQuery(String[] projectionIn, String selection, String[] selectionArgs, String groupBy,
                           String having, String sortOrder, String limit) {
    return buildQuery(projectionIn, selection, groupBy, having, sortOrder, limit);
  }

  public String buildUnionSubQuery(String typeDiscriminatorColumn, String[] unionColumns,
                                   Set<String> columnsPresentInTable, int computedColumnsOffset,
                                   String typeDiscriminatorValue, String selection, String groupBy,
                                   String having) {
    String[] projectionIn = new String[unionColumns.length];
    for (int i = 0; i < unionColumns.length; i++) {
      String c = unionColumns[i];
      if (c.equals(typeDiscriminatorColumn))
        projectionIn[i] = "'" + typeDiscriminatorValue + "' AS " + typeDiscriminatorColumn;
      else if (i <= computedColumnsOffset || columnsPresentInTable.contains(c))
        projectionIn[i] = c;
      else
        projectionIn[i] = "NULL AS " + c;
    }
    return buildQuery(projectionIn, selection, groupBy, having, null, null);
  }

  public String buildUnionQuery(String[] subQueries, String sortOrder, String limit) {
    StringBuilder query = new StringBuilder(128);
    String op = distinct ? " UNION " : " UNION ALL ";
    for (int i = 0; i < subQueries.length; i++) {
      if (i > 0) query.append(op);
      query.append(subQueries[i]);
    }
    if (sortOrder != null && !sortOrder.isEmpty()) query.append(" ORDER BY ").append(sortOrder);
    if (limit != null && !limit.isEmpty()) query.append(" LIMIT ").append(limit);
    return query.toString();
  }

  private String[] computeProjection(String[] projectionIn) {
    if (projectionIn != null && projectionIn.length > 0) {
      if (projectionMap == null) return projectionIn;
      String[] projection = new String[projectionIn.length];
      for (int i = 0; i < projectionIn.length; i++) {
        String userColumn = projectionIn[i];
        String column = projectionMap.get(userColumn);
        if (column != null) {
          projection[i] = column;
        } else if (!strict && (userColumn.contains(" AS ") || userColumn.contains(" as "))) {
          projection[i] = userColumn;
        } else {
          throw new IllegalArgumentException("Invalid column " + userColumn);
        }
      }
      return projection;
    }
    if (projectionMap != null) {
      Set<Map.Entry<String, String>> entries = projectionMap.entrySet();
      String[] projection = new String[entries.size()];
      int i = 0;
      for (Map.Entry<String, String> entry : entries) {
        if (entry.getKey().equals("_count")) continue;
        projection[i++] = entry.getValue();
      }
      return java.util.Arrays.copyOf(projection, i);
    }
    return null;
  }
}
