package android.util;

import java.util.Arrays;

/** Sorted integer keys; identity-based value lookup, like AOSP SparseArray. */
public class SparseArray<E> implements Cloneable {
  private int[] keys;
  private Object[] values;
  private int size;
  public SparseArray() { this(10); }
  public SparseArray(int capacity) { keys = new int[capacity]; values = new Object[capacity]; }
  @SuppressWarnings("unchecked") public SparseArray<E> clone() {
    try {
      SparseArray<E> copy = (SparseArray<E>) super.clone();
      copy.keys = keys.clone(); copy.values = values.clone();
      return copy;
    } catch (CloneNotSupportedException impossible) { throw new AssertionError(impossible); }
  }
  public E get(int key) { return get(key, null); }
  @SuppressWarnings("unchecked") public E get(int key, E fallback) {
    int index = indexOfKey(key);
    return index < 0 ? fallback : (E) values[index];
  }
  public void delete(int key) { remove(key); }
  public void remove(int key) { int index = indexOfKey(key); if (index >= 0) removeAt(index); }
  public void removeAt(int index) {
    checkIndex(index);
    System.arraycopy(keys, index + 1, keys, index, size - index - 1);
    System.arraycopy(values, index + 1, values, index, size - index - 1);
    values[--size] = null;
  }
  public void removeAtRange(int index, int count) {
    if (count <= 0 || index >= size) return;
    checkIndex(index);
    int removed = Math.min(count, size - index);
    System.arraycopy(keys, index + removed, keys, index, size - index - removed);
    System.arraycopy(values, index + removed, values, index, size - index - removed);
    Arrays.fill(values, size - removed, size, null);
    size -= removed;
  }
  public void put(int key, E value) {
    int index = indexOfKey(key);
    if (index >= 0) { values[index] = value; return; }
    index = ~index;
    if (size == keys.length) {
      int capacity = Math.max(4, size + (size >> 1) + 1);
      keys = Arrays.copyOf(keys, capacity); values = Arrays.copyOf(values, capacity);
    }
    System.arraycopy(keys, index, keys, index + 1, size - index);
    System.arraycopy(values, index, values, index + 1, size - index);
    keys[index] = key; values[index] = value; size++;
  }
  public void append(int key, E value) { put(key, value); }
  public int size() { return size; }
  public int keyAt(int index) { checkIndex(index); return keys[index]; }
  @SuppressWarnings("unchecked") public E valueAt(int index) { checkIndex(index); return (E) values[index]; }
  public void setValueAt(int index, E value) { checkIndex(index); values[index] = value; }
  public int indexOfKey(int key) { return Arrays.binarySearch(keys, 0, size, key); }
  public int indexOfValue(E value) {
    for (int i = 0; i < size; i++) if (values[i] == value) return i;
    return -1;
  }
  public void clear() { Arrays.fill(values, 0, size, null); size = 0; }
  private void checkIndex(int index) { if (index < 0 || index >= size) throw new ArrayIndexOutOfBoundsException(index); }
  @Override public String toString() {
    StringBuilder result = new StringBuilder("{");
    for (int i = 0; i < size; i++) {
      if (i != 0) result.append(", ");
      result.append(keys[i]).append('=').append(values[i] == this ? "(this Map)" : values[i]);
    }
    return result.append('}').toString();
  }
}
