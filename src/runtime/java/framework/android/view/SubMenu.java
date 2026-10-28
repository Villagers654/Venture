package android.view;
public interface SubMenu extends Menu {
  MenuItem getItem();
  SubMenu setHeaderTitle(CharSequence title);
  SubMenu setIcon(int icon);
  void clearHeader();
}
