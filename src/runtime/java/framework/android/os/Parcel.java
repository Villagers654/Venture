package android.os;

import java.util.ArrayList;

/** In-process parcel. Deliberately not a Binder wire-format implementation. */
public final class Parcel {
  private final ArrayList<Object> values = new ArrayList<>();
  private int position;
  public static Parcel obtain() {
    return new Parcel();
  }
  public void recycle() {
    values.clear();
    position = 0;
  }
  public int dataPosition() {
    return position;
  }
  public int dataSize() {
    return values.size();
  }
  public int dataAvail() {
    return values.size() - position;
  }
  public void setDataPosition(int p) {
    if (p < 0 || p > values.size())
      throw new IndexOutOfBoundsException();
    position = p;
  }
  private void put(Object v) {
    if (position == values.size())
      values.add(v);
    else
      values.set(position, v);
    ++position;
  }
  private Object get() {
    return values.get(position++);
  }
  public void writeInt(int v) {
    put(v);
  }
  public int readInt() {
    return (Integer) get();
  }
  public void writeLong(long v) {
    put(v);
  }
  public long readLong() {
    return (Long) get();
  }
  public void writeFloat(float v) {
    put(v);
  }
  public float readFloat() {
    return (Float) get();
  }
  public void writeDouble(double v) {
    put(v);
  }
  public double readDouble() {
    return (Double) get();
  }
  public void writeString(String v) {
    put(v);
  }
  public String readString() {
    return (String) get();
  }
  public void writeStrongBinder(IBinder binder) { put(binder); }
  public IBinder readStrongBinder() { return (IBinder) get(); }
  public void writeInterfaceToken(String descriptor) { writeString(descriptor); }
  public void enforceInterface(String descriptor) {
    if (!java.util.Objects.equals(descriptor, readString()))
      throw new SecurityException("Binder interface mismatch");
  }
  public void writeByteArray(byte[] v) {
    put(v == null ? null : v.clone());
  }
  public byte[] createByteArray() {
    byte[] v = (byte[]) get();
    return v == null ? null : v.clone();
  }
  public void writeBundle(Bundle v) {
    put(v == null ? null : new Bundle(v));
  }
  public Bundle readBundle() {
    Bundle v = (Bundle) get();
    return v == null ? null : new Bundle(v);
  }
  public Bundle readBundle(ClassLoader loader) {
    return readBundle();
  }
  public void writeParcelable(Parcelable value, int flags) {
    put(value);
  }
  @SuppressWarnings("unchecked")
  public <T extends Parcelable> T readParcelable(ClassLoader loader) {
    return (T) get();
  }
  public void writeNoException() {
    writeInt(0);
  }
  public void readException() {
    int code = readInt();
    if (code != 0)
      throw new IllegalStateException("Parcel exception " + code);
  }
  public byte[] marshall() {
    throw new UnsupportedOperationException("Cross-process parcels are unavailable");
  }
}
