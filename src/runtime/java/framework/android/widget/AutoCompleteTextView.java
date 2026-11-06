package android.widget;
public class AutoCompleteTextView extends EditText implements Filter.FilterListener {
  private ListAdapter adapter;
  private int threshold = 2;
  public AutoCompleteTextView(android.content.Context c) {
    super(c);
  }
  public AutoCompleteTextView(android.content.Context c, android.util.AttributeSet a) {
    super(c, a);
  }
  public AutoCompleteTextView(android.content.Context c, android.util.AttributeSet a, int style) {
    super(c, a, style);
  }
  public AutoCompleteTextView(android.content.Context c, android.util.AttributeSet a, int style, int res) {
    super(c, a, style);
  }
  public <T extends ListAdapter & Filterable> void setAdapter(T a) {
    adapter = a;
  }
  public ListAdapter getAdapter() {
    return adapter;
  }
  public void setThreshold(int t) {
    threshold = Math.max(1, t);
  }
  public int getThreshold() {
    return threshold;
  }
  // No drop-down is shown: Venture has no on-screen Android views.
  public boolean isPopupShowing() {
    return false;
  }
  public void showDropDown() {}
  public void dismissDropDown() {}
  public void onFilterComplete(int count) {}
}