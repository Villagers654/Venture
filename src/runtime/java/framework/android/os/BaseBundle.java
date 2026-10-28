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
}
