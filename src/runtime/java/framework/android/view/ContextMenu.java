package android.view;
public interface ContextMenu extends Menu {
  interface ContextMenuInfo {}
  ContextMenu setHeaderTitle(CharSequence title);
  void clearHeader();
}
