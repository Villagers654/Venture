package android.view;
public abstract class ActionMode {
  private Object tag;
  public void setTag(Object t) {
    tag = t;
  }
  public Object getTag() {
    return tag;
  }
  public abstract void finish();
  public abstract void invalidate();
  public abstract Menu getMenu();
  public abstract CharSequence getTitle();
  public abstract void setTitle(CharSequence title);
  public interface Callback {
    boolean onCreateActionMode(ActionMode mode, Menu menu);
    boolean onPrepareActionMode(ActionMode mode, Menu menu);
    boolean onActionItemClicked(ActionMode mode, MenuItem item);
    void onDestroyActionMode(ActionMode mode);
  }
}
