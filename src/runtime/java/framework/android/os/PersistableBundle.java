package android.os;

/** API 21 persistable values, using the framework's in-process parcel storage. */
public final class PersistableBundle extends BaseBundle implements Parcelable, Cloneable {
  private final Bundle values;
  public PersistableBundle() { values = new Bundle(); }
  public PersistableBundle(int capacity) { values = new Bundle(capacity); }
  public PersistableBundle(PersistableBundle other) { values = new Bundle(other.values); }
  private PersistableBundle(Bundle values) { this.values = new Bundle(values); }
  public Object get(String key) { return values.get(key); }
  void putValue(String key, Object value) { values.putValue(key, value); }
  public boolean containsKey(String key) { return values.containsKey(key); }
  public java.util.Set<String> keySet() { return values.keySet(); }
  public int size() { return values.size(); }
  public boolean isEmpty() { return values.isEmpty(); }
  public void clear() { values.clear(); }
  public void remove(String key) { values.remove(key); }
  public void putAll(PersistableBundle other) { values.putAll(other.values); }
  public String getString(String key) { return values.getString(key); }
  public String getString(String key, String fallback) { return values.getString(key, fallback); }
  public void putString(String key, String value) { values.putString(key, value); }
  public int getInt(String key) { return values.getInt(key); }
  public int getInt(String key, int fallback) { return values.getInt(key, fallback); }
  public void putInt(String key, int value) { values.putInt(key, value); }
  public long getLong(String key) { return values.getLong(key); }
  public long getLong(String key, long fallback) { return values.getLong(key, fallback); }
  public void putLong(String key, long value) { values.putLong(key, value); }
  public boolean getBoolean(String key) { return values.getBoolean(key); }
  public boolean getBoolean(String key, boolean fallback) { return values.getBoolean(key, fallback); }
  public void putBoolean(String key, boolean value) { values.putBoolean(key, value); }
  public double getDouble(String key) { return getDouble(key, 0); }
  public double getDouble(String key, double fallback) {
    return get(key) instanceof Double value ? value : fallback;
  }
  public void putDouble(String key, double value) { values.putDouble(key, value); }
  public void putPersistableBundle(String key, PersistableBundle value) { values.putParcelable(key, value); }
  public PersistableBundle getPersistableBundle(String key) {
    return get(key) instanceof PersistableBundle value ? value : null;
  }
  public Object clone() { return new PersistableBundle(this); }
  public int describeContents() { return 0; }
  public void writeToParcel(Parcel parcel, int flags) { parcel.writeBundle(values); }
  public static final Creator<PersistableBundle> CREATOR = new Creator<>() {
    public PersistableBundle createFromParcel(Parcel parcel) { return new PersistableBundle(parcel.readBundle()); }
    public PersistableBundle[] newArray(int size) { return new PersistableBundle[size]; }
  };
}
