package android.widget;
public class ListView extends AbsListView {
  private ListAdapter adapter;
  private int selection = -1;
  public ListView(android.content.Context c) {
    super(c);
  }
  public ListView(android.content.Context c, android.util.AttributeSet a) {
    super(c, a);
  }
  public ListView(android.content.Context c, android.util.AttributeSet a, int style) {
    super(c, a, style);
  }
  public ListView(android.content.Context c, android.util.AttributeSet a, int style, int res) {
    super(c, a, style, res);
  }
  public ListAdapter getAdapter() {
    return adapter;
  }
  public void setAdapter(ListAdapter a) {
    adapter = a;
    selection = a == null || a.getCount() == 0 ? -1 : 0;
  }
  public android.view.View getSelectedView() {
    return adapter == null || selection < 0 || selection >= adapter.getCount() ? null
        : adapter.getView(selection, null, this);
  }
  public void setSelection(int position) {
    selection = position;
  }
}
