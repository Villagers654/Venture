package android.view;
import android.content.Context;
import android.util.AttributeSet;
import java.util.*;
public class ViewGroup extends View implements ViewManager {
  @Override
  void dispatchCancelPendingInputEvents() {
    super.dispatchCancelPendingInputEvents();
    for (View child : new ArrayList<>(children)) child.dispatchCancelPendingInputEvents();
  }
  public void updateViewLayout(View v, LayoutParams p) {
    if (v.getParent() != this)
      throw new IllegalArgumentException("Not a child");
    v.setLayoutParams(p);
    v.requestLayout();
  }
  protected final List<View> children = new ArrayList<>();
  public ViewGroup(Context c) {
    super(c);
  }
  public ViewGroup(Context c, AttributeSet a) {
    super(c, a);
  }
  public ViewGroup(Context c, AttributeSet a, int s) {
    super(c, a, s);
  }
  public static class LayoutParams {
    public static final int MATCH_PARENT = -1, FILL_PARENT = -1, WRAP_CONTENT = -2;
    public int width, height;
    public LayoutParams(int w, int h) {
      width = w;
      height = h;
    }
    public LayoutParams(LayoutParams p) {
      this(p.width, p.height);
    }
  }
  public static class MarginLayoutParams extends LayoutParams {
    public int leftMargin, topMargin, rightMargin, bottomMargin;
    public MarginLayoutParams(int w, int h) {
      super(w, h);
    }
    public void setMargins(int l, int t, int r, int b) {
      leftMargin = l;
      topMargin = t;
      rightMargin = r;
      bottomMargin = b;
    }
  }
  public void addView(View v) {
    addView(v, children.size(), new LayoutParams(-1, -1));
  }
  public void addView(View v, LayoutParams p) {
    addView(v, children.size(), p);
  }
  public void addView(View v, int index) {
    addView(v, index, new LayoutParams(-1, -1));
  }
  public void addView(View v, int w, int h) {
    addView(v, new LayoutParams(w, h));
  }
  public void addView(View v, int index, LayoutParams p) {
    if (v.parent != null)
      throw new IllegalStateException("View already has parent");
    v.parent = this;
    v.setLayoutParams(p);
    children.add(index < 0 ? children.size() : index, v);
    if (isAttachedToWindow())
      v.attach();
  }
  public void removeView(View v) {
    if (children.remove(v)) {
      v.detach();
      v.parent = null;
    }
  }
  public void removeAllViews() {
    for (View v : List.copyOf(children)) removeView(v);
  }
  public int getChildCount() {
    return children.size();
  }
  public View getChildAt(int i) {
    return children.get(i);
  }
  public void bringChildToFront(View v) {
    if (children.remove(v))
      children.add(v);
  }
  public <T extends View> T findViewById(int id) {
    T self = super.findViewById(id);
    if (self != null)
      return self;
    for (View v : children) {
      T found = v.findViewById(id);
      if (found != null)
        return found;
    }
    return null;
  }
  public void attach() {
    super.attach();
    for (View v : List.copyOf(children)) v.attach();
  }
  void seedMeasurement() {
    super.seedMeasurement();
    for (View v : List.copyOf(children)) v.seedMeasurement();
  }
  public void detach() {
    for (View v : List.copyOf(children)) v.detach();
    super.detach();
  }
  public void onWindowFocusChanged(boolean focused) {
    super.onWindowFocusChanged(focused);
    for (View v : List.copyOf(children)) v.onWindowFocusChanged(focused);
  }
}
