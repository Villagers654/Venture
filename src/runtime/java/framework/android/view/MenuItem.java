package android.view;
public interface MenuItem {
  int getItemId();
  int getGroupId();
  CharSequence getTitle();
  MenuItem setTitle(CharSequence title);
  MenuItem setVisible(boolean visible);
  boolean isVisible();
  MenuItem setEnabled(boolean enabled);
  boolean isEnabled();
  MenuItem setOnMenuItemClickListener(OnMenuItemClickListener listener);
  interface OnMenuItemClickListener {
    boolean onMenuItemClick(MenuItem item);
  }
  interface OnActionExpandListener {
    boolean onMenuItemActionExpand(MenuItem item);
    boolean onMenuItemActionCollapse(MenuItem item);
  }
}
