package android.view;
public interface Menu {
  MenuItem add(CharSequence title);
  MenuItem add(int titleRes);
  MenuItem add(int groupId, int itemId, int order, CharSequence title);
  MenuItem findItem(int id);
  void clear();
  void removeItem(int id);
  int size();
  MenuItem getItem(int index);
  boolean hasVisibleItems();
  void close();
}
