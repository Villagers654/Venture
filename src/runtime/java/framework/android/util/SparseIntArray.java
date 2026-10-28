package android.util;

import java.util.Arrays;

/** Sorted integer map; indices follow AOSP android/util/SparseIntArray.java. */
public class SparseIntArray implements Cloneable {
  private int[] keys, values;
  private int size;
  public SparseIntArray() { this(10); }
  public SparseIntArray(int capacity) {
    keys = new int[capacity];
    values = new int[capacity];
  }
  public SparseIntArray clone() {
    try {
      var copy = (SparseIntArray) super.clone();
      copy.keys = keys.clone();
      copy.values = values.clone();
      return copy;
    } catch (CloneNotSupportedException impossible) { throw new AssertionError(impossible); }
  }
  public int get(int key) { return get(key, 0); }
  public int get(int key, int fallback) {
    int index = indexOfKey(key);
    return index < 0 ? fallback : values[index];
  }
  public int size() { return size; }
  public int keyAt(int index) { checkIndex(index); return keys[index]; }
  public int valueAt(int index) { checkIndex(index); return values[index]; }
  public void setValueAt(int index, int value) { checkIndex(index); values[index] = value; }
  private void checkIndex(int index) {
    if (index < 0 || index >= size) throw new ArrayIndexOutOfBoundsException(index);
  }
  public int indexOfKey(int key) { return Arrays.binarySearch(keys, 0, size, key); }
  public int indexOfValue(int value) {
    for (int i = 0; i < size; ++i) if (values[i] == value) return i;
    return -1;
  }
  public void put(int key, int value) {
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
  public void append(int key, int value) { put(key, value); }
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
