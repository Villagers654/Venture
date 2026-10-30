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
  public float getFloat(String k) { return getFloat(k, 0); }
  public float getFloat(String k, float d) {
    Object v = get(k);
    return v instanceof Float ? (Float) v : d;
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
  public void putParcelable(String k, Parcelable v) {
    values.put(k, v);
  }
  @SuppressWarnings("unchecked")
  public <T extends Parcelable> T getParcelable(String k) {
    return (T) get(k);
  }
  void putValue(String k, Object v) { values.put(k, v); }
  public ClassLoader getClassLoader() { return com.venture.runtime.RuntimeEnvironment.get().loader; }
  public void putByte(String k, byte v) { values.put(k, v); }
  public void putChar(String k, char v) { values.put(k, v); }
  public void putShort(String k, short v) { values.put(k, v); }
  public void putCharArray(String k, char[] v) { values.put(k, v); }
  public void putShortArray(String k, short[] v) { values.put(k, v); }
  public void putFloatArray(String k, float[] v) { values.put(k, v); }
  public void putCharSequenceArray(String k, CharSequence[] v) { values.put(k, v); }
  public void putParcelableArray(String k, Parcelable[] v) { values.put(k, v); }
  public void putIntegerArrayList(String k, ArrayList<Integer> v) { values.put(k, v); }
  public void putStringArrayList(String k, ArrayList<String> v) { values.put(k, v); }
  public void putCharSequenceArrayList(String k, ArrayList<CharSequence> v) { values.put(k, v); }
  public void putParcelableArrayList(String k, ArrayList<? extends Parcelable> v) { values.put(k, v); }
  public void putSparseParcelableArray(String k, android.util.SparseArray<? extends Parcelable> v) { values.put(k, v); }
  public void putSize(String k, android.util.Size v) { values.put(k, v); }
  public void putSizeF(String k, android.util.SizeF v) { values.put(k, v); }
  public void putBinder(String k, IBinder v) { values.put(k, v); }
  public byte getByte(String k) { return getByte(k, (byte) 0); }
  public Byte getByte(String k, byte d) { return get(k) instanceof Byte v ? v : d; }
  public char getChar(String k) { return getChar(k, (char) 0); }
  public char getChar(String k, char d) { return get(k) instanceof Character v ? v : d; }
  public short getShort(String k) { return getShort(k, (short) 0); }
  public short getShort(String k, short d) { return get(k) instanceof Short v ? v : d; }
  public char[] getCharArray(String k) { return get(k) instanceof char[] v ? v : null; }
  public short[] getShortArray(String k) { return get(k) instanceof short[] v ? v : null; }
  public float[] getFloatArray(String k) { return get(k) instanceof float[] v ? v : null; }
  public CharSequence[] getCharSequenceArray(String k) { return get(k) instanceof CharSequence[] v ? v : null; }
  public Parcelable[] getParcelableArray(String k) { return get(k) instanceof Parcelable[] v ? v : null; }
  @SuppressWarnings("unchecked")
  public <T> T[] getParcelableArray(String k, Class<T> type) {
    Parcelable[] all = getParcelableArray(k);
    if (all == null) return null;
    T[] out = (T[]) java.lang.reflect.Array.newInstance(type, all.length);
    for (int i = 0; i < all.length; i++) {
      if (all[i] != null && !type.isInstance(all[i])) return null;
      out[i] = type.cast(all[i]);
    }
    return out;
  }
  @SuppressWarnings("unchecked")
  private <T> ArrayList<T> list(String k) { return get(k) instanceof ArrayList<?> v ? (ArrayList<T>) v : null; }
  public ArrayList<Integer> getIntegerArrayList(String k) { return list(k); }
  public ArrayList<String> getStringArrayList(String k) { return list(k); }
  public ArrayList<CharSequence> getCharSequenceArrayList(String k) { return list(k); }
  public <T extends Parcelable> ArrayList<T> getParcelableArrayList(String k) { return list(k); }
  public <T> ArrayList<T> getParcelableArrayList(String k, Class<? extends T> type) {
    ArrayList<T> all = list(k);
    if (all != null)
      for (T value : all)
        if (value != null && !type.isInstance(value)) return null;
    return all;
  }
  @SuppressWarnings("unchecked")
  public <T extends Parcelable> android.util.SparseArray<T> getSparseParcelableArray(String k) {
    return get(k) instanceof android.util.SparseArray<?> v ? (android.util.SparseArray<T>) v : null;
  }
  @SuppressWarnings("unchecked")
  public <T> android.util.SparseArray<T> getSparseParcelableArray(String k, Class<? extends T> type) {
    return get(k) instanceof android.util.SparseArray<?> v ? (android.util.SparseArray<T>) v : null;
  }
  public android.util.Size getSize(String k) { return get(k) instanceof android.util.Size v ? v : null; }
  public android.util.SizeF getSizeF(String k) { return get(k) instanceof android.util.SizeF v ? v : null; }
  public IBinder getBinder(String k) { return get(k) instanceof IBinder v ? v : null; }
  public <T> T getParcelable(String k, Class<T> type) {
    Object value = get(k);
    return type.isInstance(value) ? type.cast(value) : null;
  }
  public <T extends java.io.Serializable> T getSerializable(String k, Class<T> type) {
    Object value = get(k);
    return type.isInstance(value) ? type.cast(value) : null;
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
