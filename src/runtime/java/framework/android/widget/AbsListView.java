package android.widget;
// AOSP ancestry and listener interfaces, so app subclasses (appcompat's DropDownListView) load
// and verify. There is no on-screen list: items come from the adapter on request.
public abstract class AbsListView extends AdapterView<ListAdapter>
    implements android.text.TextWatcher, android.view.ViewTreeObserver.OnGlobalLayoutListener,
               Filter.FilterListener, android.view.ViewTreeObserver.OnTouchModeChangeListener {
  public interface OnScrollListener {
    int SCROLL_STATE_IDLE = 0;
    int SCROLL_STATE_TOUCH_SCROLL = 1;
    int SCROLL_STATE_FLING = 2;
    void onScrollStateChanged(AbsListView view, int scrollState);
    void onScroll(AbsListView view, int firstVisibleItem, int visibleItemCount, int totalItemCount);
  }
  public interface SelectionBoundsAdjuster {
    void adjustListItemSelectionBounds(android.graphics.Rect bounds);
  }
  public interface RecyclerListener {
    void onMovedToScrapHeap(android.view.View view);
  }
  public static final int CHOICE_MODE_NONE = 0;
  public static final int CHOICE_MODE_SINGLE = 1;
  public static final int CHOICE_MODE_MULTIPLE = 2;
  public static final int CHOICE_MODE_MULTIPLE_MODAL = 3;
  private int choiceMode;
  private OnScrollListener scrollListener;
  public void setOnScrollListener(OnScrollListener l) {
    scrollListener = l;
  }
  public AbsListView(android.content.Context c) {
    super(c);
  }
  public AbsListView(android.content.Context c, android.util.AttributeSet a) {
    super(c, a);
  }
  public AbsListView(android.content.Context c, android.util.AttributeSet a, int style) {
    super(c, a, style);
  }
  public AbsListView(android.content.Context c, android.util.AttributeSet a, int style, int res) {
    super(c, a, style);
  }
  public void setChoiceMode(int mode) {
    choiceMode = mode;
  }
  public int getChoiceMode() {
    return choiceMode;
  }
  public int getCount() {
    ListAdapter adapter = getAdapter();
    return adapter == null ? 0 : adapter.getCount();
  }
  public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
  public void onTextChanged(CharSequence s, int start, int before, int count) {}
  public void afterTextChanged(android.text.Editable text) {}
  public void onGlobalLayout() {}
  public void onFilterComplete(int count) {}
  public void onTouchModeChanged(boolean isInTouchMode) {}
}
