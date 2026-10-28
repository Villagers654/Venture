package android.os;

import java.util.*;

public class Bundle extends BaseBundle implements Parcelable, Cloneable {
  public static final Bundle EMPTY = new Bundle(Collections.emptyMap());
  private final Map<String, Object> values;
  public Bundle() {
    values = new HashMap<>();
  }
  public Bundle(int capacity) {
    values = new HashMap<>(capacity);
  }
  public Bundle(Bundle other) {
    values = new HashMap<>(other.values);
  }
  private Bundle(Map<String, Object> map) {
    values = map;
  }
  public Object get(String key) {
    return values.get(key);
  }
  public void putAll(Bundle other) {
    values.putAll(other.values);
  }
  public boolean containsKey(String key) {
    return values.containsKey(key);
  }
  public Set<String> keySet() {
    return Collections.unmodifiableSet(values.keySet());
  }
  public int size() {
    return values.size();
  }
  public boolean isEmpty() {
    return values.isEmpty();
  }
  public void clear() {
    values.clear();
  }
  public void remove(String key) {
    values.remove(key);
  }
  // All payloads are live objects; there is no lazy parcel deserialization.
  public void setClassLoader(ClassLoader loader) {}
  public void putString(String k, String v) {
    values.put(k, v);
  }
  public void putCharSequence(String k, CharSequence v) { values.put(k, v); }
  public CharSequence getCharSequence(String k) { return getCharSequence(k, null); }
  public CharSequence getCharSequence(String k, CharSequence fallback) {
    Object value = get(k); return value instanceof CharSequence chars ? chars : fallback;
  }
  public String getString(String k) {
    return getString(k, null);
  }
  public String getString(String k, String d) {
    Object v = get(k);
    return v instanceof String ? (String) v : d;
  }
  public void putInt(String k, int v) {
    values.put(k, v);
  }
  public int getInt(String k) {
    return getInt(k, 0);
  }
  public int getInt(String k, int d) {
    Object v = get(k);
    return v instanceof Integer ? (Integer) v : d;
  }
  public void putLong(String k, long v) {
    values.put(k, v);
  }
  public long getLong(String k) {
    return getLong(k, 0);
  }
  public long getLong(String k, long d) {
    Object v = get(k);
    return v instanceof Long ? (Long) v : d;
  }
  public void putBoolean(String k, boolean v) {
    values.put(k, v);
  }
  public boolean getBoolean(String k) {
    return getBoolean(k, false);
  }
  public boolean getBoolean(String k, boolean d) {
    Object v = get(k);
    return v instanceof Boolean ? (Boolean) v : d;
  }
  public void putFloat(String k, float v) {
    values.put(k, v);
  }
  public float getFloat(String k) {
    Object v = get(k);
    return v instanceof Float ? (Float) v : 0;
  }
  public void putDouble(String k, double v) {
    values.put(k, v);
  }
  public double getDouble(String k) {
    return getDouble(k, 0);
  }
  public double getDouble(String k, double d) {
    Object v = get(k);
    return v instanceof Double ? (Double) v : d;
  }
  public void putBundle(String k, Bundle v) {
    values.put(k, v);
  }
  public Bundle getBundle(String k) {
    return (Bundle) get(k);
  }
  public void putByteArray(String k, byte[] v) {
    values.put(k, v);
  }
  public byte[] getByteArray(String k) {
    return (byte[]) get(k);
  }
  public void putStringArray(String k, String[] v) {
    values.put(k, v);
  }
  public String[] getStringArray(String k) {
    return (String[]) get(k);
  }
  public void putIntArray(String k, int[] v) {
    values.put(k, v);
  }
  public int[] getIntArray(String k) {
    return (int[]) get(k);
  }
  public void putParcelable(String k, Parcelable v) {
    values.put(k, v);
  }
  @SuppressWarnings("unchecked")
  public <T extends Parcelable> T getParcelable(String k) {
    return (T) get(k);
  }
  public void putSerializable(String k, java.io.Serializable v) {
    values.put(k, v);
  }
  public java.io.Serializable getSerializable(String k) {
    return (java.io.Serializable) get(k);
  }
  public Object clone() {
    return new Bundle(this);
  }
  public int describeContents() {
    return 0;
  }
  public void writeToParcel(Parcel out, int flags) {
    out.writeBundle(this);
  }
  public static final Creator<Bundle> CREATOR = new Creator<>() {
    public Bundle createFromParcel(Parcel p) {
      return p.readBundle();
    }
    public Bundle[] newArray(int n) {
      return new Bundle[n];
    }
  };
}
