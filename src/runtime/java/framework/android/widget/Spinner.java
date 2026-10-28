package android.widget;
public class Spinner extends AdapterView<SpinnerAdapter> {
  private SpinnerAdapter adapter;
  private int selection;
  public Spinner(android.content.Context c) {
    super(c);
  }
  public Spinner(android.content.Context c, int mode) {
    super(c);
  }
  public SpinnerAdapter getAdapter() {
    return adapter;
  }
  public void setAdapter(SpinnerAdapter a) {
    adapter = a;
    selection = 0;
  }
  public android.view.View getSelectedView() {
    return adapter == null || adapter.getCount() == 0 ? null
                                                      : adapter.getView(selection, null, this);
  }
  public void setSelection(int p) {
    if (adapter == null || p < 0 || p >= adapter.getCount())
      throw new IndexOutOfBoundsException();
    selection = p;
  }
}
