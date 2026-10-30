package android.util;

import java.util.Arrays;

/** Sorted map of int keys to boolean values; indices follow AOSP android/util/SparseBooleanArray.java. */
public class SparseBooleanArray implements Cloneable {
  private int[] keys;
  private boolean[] values;
  private int size;
  public SparseBooleanArray() { this(10); }
  public SparseBooleanArray(int capacity) {
    keys = new int[capacity];
    values = new boolean[capacity];
  }
  public SparseBooleanArray clone() {
    try {
      var copy = (SparseBooleanArray) super.clone();
      copy.keys = keys.clone();
      copy.values = values.clone();
      return copy;
    } catch (CloneNotSupportedException impossible) { throw new AssertionError(impossible); }
  }
  public boolean get(int key) { return get(key, false); }
  public boolean get(int key, boolean fallback) {
    int index = indexOfKey(key);
    return index < 0 ? fallback : values[index];
  }
  public int size() { return size; }
  public int keyAt(int index) { checkIndex(index); return keys[index]; }
  public boolean valueAt(int index) { checkIndex(index); return values[index]; }
  public void setValueAt(int index, boolean value) { checkIndex(index); values[index] = value; }
  private void checkIndex(int index) {
    if (index < 0 || index >= size) throw new ArrayIndexOutOfBoundsException(index);
  }
  public int indexOfKey(int key) { return Arrays.binarySearch(keys, 0, size, key); }
  public int indexOfValue(boolean value) {
    for (int i = 0; i < size; ++i) if (values[i] == value) return i;
    return -1;
  }
  public void put(int key, boolean value) {
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
  public void append(int key, boolean value) { put(key, value); }
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
