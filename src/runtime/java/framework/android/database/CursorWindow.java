package android.database;

import android.database.sqlite.SQLiteException;
import java.io.Closeable;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Locale;
import java.util.regex.Pattern;

/**
 * AOSP android.database.CursorWindow: a block of typed result rows. AOSP keeps
 * the rows in native shared memory; here they are Java values (Long, Double,
 * String, byte[] or null) with the per-type conversions of
 * android_database_CursorWindow.cpp (strtoll/strtod, "%g", BLOB errors).
 */
public class CursorWindow implements Closeable {
  private static final Pattern DOUBLE_PREFIX =
      Pattern.compile("^\\s*([+-]?(\\d+\\.?\\d*|\\.\\d+)([eE][+-]?\\d+)?)");
  private final String name;
  private final ArrayList<Object[]> rows = new ArrayList<>();
  private int columns, start;
  private boolean closed;

  public CursorWindow(String name) { this.name = name == null || name.isEmpty() ? "<unnamed>" : name; }
  @Deprecated public CursorWindow(boolean localWindow) { this((String) null); }
  public String getName() { return name; }
  public synchronized void clear() { rows.clear(); columns = 0; start = 0; }
  public synchronized int getStartPosition() { return start; }
  public synchronized void setStartPosition(int pos) { start = pos; }
  public synchronized int getNumRows() { return rows.size(); }
  public synchronized boolean setNumColumns(int columnNum) {
    if (!rows.isEmpty() && columnNum != columns) return false;
    columns = columnNum;
    return true;
  }
  public synchronized boolean allocRow() {
    rows.add(new Object[columns]);
    return true;
  }
  public synchronized void freeLastRow() { if (!rows.isEmpty()) rows.remove(rows.size() - 1); }
  public boolean putBlob(byte[] value, int row, int column) { return put(value == null ? null : value.clone(), row, column); }
  public boolean putString(String value, int row, int column) { return put(value, row, column); }
  public boolean putLong(long value, int row, int column) { return put(value, row, column); }
  public boolean putDouble(double value, int row, int column) { return put(value, row, column); }
  public boolean putNull(int row, int column) { return put(null, row, column); }
  private synchronized boolean put(Object value, int row, int column) {
    int r = row - start;
    if (r < 0 || r >= rows.size() || column < 0 || column >= columns) return false;
    rows.get(r)[column] = value;
    return true;
  }
  private synchronized Object field(int row, int column) {
    if (closed) throw new IllegalStateException("CursorWindow " + name + " is closed");
    int r = row - start;
    if (r < 0 || r >= rows.size() || column < 0 || column >= columns)
      throw new IllegalStateException("Couldn't read row " + row + ", col " + column + " from CursorWindow. "
          + "Make sure the Cursor is initialized correctly before accessing data from it.");
    return rows.get(r)[column];
  }
  public int getType(int row, int column) {
    Object v = field(row, column);
    if (v == null) return Cursor.FIELD_TYPE_NULL;
    if (v instanceof Long) return Cursor.FIELD_TYPE_INTEGER;
    if (v instanceof Double) return Cursor.FIELD_TYPE_FLOAT;
    if (v instanceof String) return Cursor.FIELD_TYPE_STRING;
    return Cursor.FIELD_TYPE_BLOB;
  }
  public boolean isNull(int row, int column) { return field(row, column) == null; }
  public byte[] getBlob(int row, int column) {
    Object v = field(row, column);
    if (v == null) return null;
    if (v instanceof byte[] b) return b.clone();
    if (v instanceof String s) {  // AOSP returns the stored string with its terminator
      byte[] utf = s.getBytes(StandardCharsets.UTF_8);
      return Arrays.copyOf(utf, utf.length + 1);
    }
    throw new SQLiteException(v instanceof Long ? "INTEGER data in nativeGetBlob " : "FLOAT data in nativeGetBlob ");
  }
  public String getString(int row, int column) {
    Object v = field(row, column);
    if (v == null || v instanceof String) return (String) v;
    if (v instanceof Long l) return Long.toString(l);
    if (v instanceof Double d) return formatG(d);
    throw new SQLiteException("Unable to convert BLOB to string");
  }
  public long getLong(int row, int column) {
    Object v = field(row, column);
    if (v == null) return 0;
    if (v instanceof Long l) return l;
    if (v instanceof Double d) return (long) (double) d;
    if (v instanceof String s) return parseLong(s);
    throw new SQLiteException("Unable to convert BLOB to long");
  }
  public double getDouble(int row, int column) {
    Object v = field(row, column);
    if (v == null) return 0;
    if (v instanceof Double d) return d;
    if (v instanceof Long l) return l;
    if (v instanceof String s) return parseDouble(s);
    throw new SQLiteException("Unable to convert BLOB to double");
  }
  public short getShort(int row, int column) { return (short) getLong(row, column); }
  public int getInt(int row, int column) { return (int) getLong(row, column); }
  public float getFloat(int row, int column) { return (float) getDouble(row, column); }
  // strtoll(s, NULL, 0): C-locale space, a sign, then "0x" hex, leading-zero
  // octal or decimal ASCII digits; out-of-range values clamp to LLONG_MIN/MAX.
  private static long parseLong(String s) {
    int i = 0, n = s.length();
    while (i < n && (s.charAt(i) == ' ' || (s.charAt(i) >= '\t' && s.charAt(i) <= '\r'))) ++i;
    boolean negative = false;
    if (i < n && (s.charAt(i) == '+' || s.charAt(i) == '-')) negative = s.charAt(i++) == '-';
    int base = 10;
    if (i + 2 < n && s.charAt(i) == '0' && (s.charAt(i + 1) | 0x20) == 'x' && digit(s.charAt(i + 2), 16) >= 0) {
      base = 16;
      i += 2;
    } else if (i < n && s.charAt(i) == '0') {
      base = 8;
    }
    // Accumulate negatively, as Long.parseLong does, so LLONG_MIN is representable.
    long limit = negative ? Long.MIN_VALUE : -Long.MAX_VALUE, value = 0;
    boolean overflow = false;
    for (int d; i < n && (d = digit(s.charAt(i), base)) >= 0; ++i) {
      if (overflow || value < limit / base || value * base < limit + d) overflow = true;
      else value = value * base - d;
    }
    if (overflow) return negative ? Long.MIN_VALUE : Long.MAX_VALUE;
    return negative ? value : -value;
  }
  private static int digit(char c, int base) {
    int d = c >= '0' && c <= '9' ? c - '0' : (c | 0x20) >= 'a' && (c | 0x20) <= 'z' ? (c | 0x20) - 'a' + 10 : -1;
    return d < base ? d : -1;
  }
  private static double parseDouble(String s) {
    var m = DOUBLE_PREFIX.matcher(s);
    return m.find() ? Double.parseDouble(m.group(1)) : 0;
  }
  // C printf "%g": six significant digits, trailing zeros removed.
  static String formatG(double d) {
    if (Double.isNaN(d)) return "nan";
    if (Double.isInfinite(d)) return d > 0 ? "inf" : "-inf";
    if (d == 0) return 1 / d < 0 ? "-0" : "0";
    String sci = String.format(Locale.ROOT, "%.5e", d);
    int e = sci.indexOf('e');
    int exponent = Integer.parseInt(sci.substring(e + 1));
    if (exponent < -4 || exponent >= 6) {
      int magnitude = Math.abs(exponent);
      return stripZeros(sci.substring(0, e)) + "e" + (exponent < 0 ? "-" : "+") + (magnitude < 10 ? "0" : "") + magnitude;
    }
    return stripZeros(String.format(Locale.ROOT, "%." + (5 - exponent) + "f", d));
  }
  private static String stripZeros(String s) {
    if (s.indexOf('.') < 0) return s;
    int end = s.length();
    while (s.charAt(end - 1) == '0') --end;
    if (s.charAt(end - 1) == '.') --end;
    return s.substring(0, end);
  }
  @Override public synchronized void close() { closed = true; rows.clear(); }
  @Override public String toString() { return name + " {" + getNumRows() + " rows}"; }
}
