package android.util;

import java.util.Arrays;

/** Sorted map of int keys to long values; indices follow AOSP android/util/SparseLongArray.java. */
public class SparseLongArray implements Cloneable {
  private int[] keys;
  private long[] values;
  private int size;
  public SparseLongArray() { this(10); }
  public SparseLongArray(int capacity) {
    keys = new int[capacity];
    values = new long[capacity];
  }
  public SparseLongArray clone() {
    try {
      var copy = (SparseLongArray) super.clone();
      copy.keys = keys.clone();
      copy.values = values.clone();
      return copy;
    } catch (CloneNotSupportedException impossible) { throw new AssertionError(impossible); }
  }
  public long get(int key) { return get(key, 0); }
  public long get(int key, long fallback) {
    int index = indexOfKey(key);
    return index < 0 ? fallback : values[index];
  }
  public int size() { return size; }
  public int keyAt(int index) { checkIndex(index); return keys[index]; }
  public long valueAt(int index) { checkIndex(index); return values[index]; }
  public void setValueAt(int index, long value) { checkIndex(index); values[index] = value; }
  private void checkIndex(int index) {
    if (index < 0 || index >= size) throw new ArrayIndexOutOfBoundsException(index);
  }
  public int indexOfKey(int key) { return Arrays.binarySearch(keys, 0, size, key); }
  public int indexOfValue(long value) {
    for (int i = 0; i < size; ++i) if (values[i] == value) return i;
    return -1;
  }
  public void put(int key, long value) {
    int index = indexOfKey(key);
    if (index >= 0) { values[index] = value; return; }
    index = ~index;
    if (size == keys.length) {
      int capacity = Math.max(8, Math.addExact(size, size));
      keys = Arrays.copyOf(keys, capacity);
      values = Arrays.copyOf(values, capacity);
    }
    System.arraycopy(keys, index, keys, index + 1, size - index);
    System.arraycopy(values, index, values, index + 1, size - index);
    keys[index] = key;
    values[index] = value;
    ++size;
  }
  public void append(int key, long value) { put(key, value); }
  public void delete(int key) {
    int index = indexOfKey(key);
    if (index >= 0) removeAt(index);
  }
  public void removeAt(int index) {
    checkIndex(index);
    System.arraycopy(keys, index + 1, keys, index, size - index - 1);
    System.arraycopy(values, index + 1, values, index, size - index - 1);
    --size;
  }
  public void clear() { size = 0; }
  public String toString() {
    var out = new StringBuilder("{");
    for (int i = 0; i < size; ++i) {
      if (i != 0) out.append(", ");
      out.append(keys[i]).append('=').append(values[i]);
    }
    return out.append('}').toString();
  }
}
