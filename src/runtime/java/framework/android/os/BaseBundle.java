package android.os;
public abstract class BaseBundle {
  public abstract Object get(String key);
  public abstract String getString(String key);
  public abstract String getString(String key, String fallback);
  public abstract int getInt(String key);
  public abstract boolean getBoolean(String key);
  public abstract boolean containsKey(String key);
  public abstract java.util.Set<String> keySet();
  public abstract int size();
  public abstract boolean isEmpty();
  public abstract void clear();
  public abstract void remove(String key);
  public abstract int getInt(String key, int defaultValue);
  public abstract long getLong(String key);
  public abstract long getLong(String key, long defaultValue);
  public abstract boolean getBoolean(String key, boolean defaultValue);
  public abstract double getDouble(String key);
  public abstract double getDouble(String key, double defaultValue);
  public abstract void putBoolean(String key, boolean value);
  public abstract void putInt(String key, int value);
  public abstract void putLong(String key, long value);
  public abstract void putDouble(String key, double value);
  public abstract void putString(String key, String value);
  abstract void putValue(String key, Object value);
  // AOSP BaseBundle: a value of another type reads as null.
  public boolean[] getBooleanArray(String key) { return get(key) instanceof boolean[] v ? v : null; }
  public int[] getIntArray(String key) { return get(key) instanceof int[] v ? v : null; }
  public long[] getLongArray(String key) { return get(key) instanceof long[] v ? v : null; }
  public double[] getDoubleArray(String key) { return get(key) instanceof double[] v ? v : null; }
  public String[] getStringArray(String key) { return get(key) instanceof String[] v ? v : null; }
  public void putBooleanArray(String key, boolean[] value) { putValue(key, value); }
  public void putIntArray(String key, int[] value) { putValue(key, value); }
  public void putLongArray(String key, long[] value) { putValue(key, value); }
  public void putDoubleArray(String key, double[] value) { putValue(key, value); }
  public void putStringArray(String key, String[] value) { putValue(key, value); }
}
