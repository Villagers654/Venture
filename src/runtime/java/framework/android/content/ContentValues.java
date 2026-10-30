package android.content;

import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Set;

/** AOSP android.content.ContentValues: column values for a database insert or update. */
public final class ContentValues {
  private final LinkedHashMap<String, Object> values;
  public ContentValues() { values = new LinkedHashMap<>(); }
  public ContentValues(int size) { values = new LinkedHashMap<>(Math.max(0, size)); }
  public ContentValues(ContentValues from) { values = new LinkedHashMap<>(from.values); }
  public void put(String key, String value) { values.put(key, value); }
  public void put(String key, Byte value) { values.put(key, value); }
  public void put(String key, Short value) { values.put(key, value); }
  public void put(String key, Integer value) { values.put(key, value); }
  public void put(String key, Long value) { values.put(key, value); }
  public void put(String key, Float value) { values.put(key, value); }
  public void put(String key, Double value) { values.put(key, value); }
  public void put(String key, Boolean value) { values.put(key, value); }
  public void put(String key, byte[] value) { values.put(key, value); }
  public void putNull(String key) { values.put(key, null); }
  public void putAll(ContentValues other) { values.putAll(other.values); }
  public int size() { return values.size(); }
  public boolean isEmpty() { return values.isEmpty(); }
  public void remove(String key) { values.remove(key); }
  public void clear() { values.clear(); }
  public boolean containsKey(String key) { return values.containsKey(key); }
  public Object get(String key) { return values.get(key); }
  public String getAsString(String key) { Object value = values.get(key); return value != null ? value.toString() : null; }
  public Long getAsLong(String key) {
    Object value = values.get(key);
    if (value instanceof Number n) return n.longValue();
    try { return value != null ? Long.valueOf(value.toString()) : null; } catch (NumberFormatException e) { return null; }
  }
  public Integer getAsInteger(String key) {
    Object value = values.get(key);
    if (value instanceof Number n) return n.intValue();
    try { return value != null ? Integer.valueOf(value.toString()) : null; } catch (NumberFormatException e) { return null; }
  }
  public Short getAsShort(String key) { Long value = getAsLong(key); return value != null ? value.shortValue() : null; }
  public Byte getAsByte(String key) { Long value = getAsLong(key); return value != null ? value.byteValue() : null; }
  public Double getAsDouble(String key) {
    Object value = values.get(key);
    if (value instanceof Number n) return n.doubleValue();
    try { return value != null ? Double.valueOf(value.toString()) : null; } catch (NumberFormatException e) { return null; }
  }
  public Float getAsFloat(String key) { Double value = getAsDouble(key); return value != null ? value.floatValue() : null; }
  public Boolean getAsBoolean(String key) {
    Object value = values.get(key);
    if (value instanceof Boolean b) return b;
    if (value instanceof Number n) return n.intValue() != 0;
    return value instanceof CharSequence s ? Boolean.valueOf(s.toString()) || "1".equals(s.toString()) : null;
  }
  public byte[] getAsByteArray(String key) { return values.get(key) instanceof byte[] b ? b : null; }
  public Set<Map.Entry<String, Object>> valueSet() { return values.entrySet(); }
  public Set<String> keySet() { return values.keySet(); }
  @Override public boolean equals(Object other) { return other instanceof ContentValues c && values.equals(c.values); }
  @Override public int hashCode() { return values.hashCode(); }
  @Override public String toString() {
    var text = new StringBuilder();
    for (var entry : values.entrySet()) {
      if (text.length() > 0) text.append(' ');
      text.append(entry.getKey()).append('=').append(entry.getValue());
    }
    return text.toString();
  }
}
