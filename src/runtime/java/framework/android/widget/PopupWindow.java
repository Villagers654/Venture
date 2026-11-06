package android.widget;
// Venture has no on-screen Android views: a popup records its content and state only.
public class PopupWindow {
  public interface OnDismissListener {
    void onDismiss();
  }
  private android.view.View content;
  private int width, height;
  private boolean showing, focusable, touchable = true, outsideTouchable;
  private OnDismissListener dismissListener;
  private android.graphics.drawable.Drawable background;
  public PopupWindow() {}
  public PopupWindow(android.content.Context c) {}
  public PopupWindow(android.content.Context c, android.util.AttributeSet a) {}
  public PopupWindow(android.content.Context c, android.util.AttributeSet a, int style) {}
  public PopupWindow(android.content.Context c, android.util.AttributeSet a, int style, int res) {}
  public PopupWindow(android.view.View content) { this(content, 0, 0); }
  public PopupWindow(int width, int height) { this(null, width, height); }
  public PopupWindow(android.view.View content, int width, int height) { this(content, width, height, false); }
  public PopupWindow(android.view.View content, int width, int height, boolean focusable) {
    this.content = content;
    this.width = width;
    this.height = height;
    this.focusable = focusable;
  }
  public void setContentView(android.view.View v) { content = v; }
  public android.view.View getContentView() { return content; }
  public void setWidth(int w) { width = w; }
  public int getWidth() { return width; }
  public void setHeight(int h) { height = h; }
  public int getHeight() { return height; }
  public void setFocusable(boolean f) { focusable = f; }
  public boolean isFocusable() { return focusable; }
  public void setTouchable(boolean t) { touchable = t; }
  public boolean isTouchable() { return touchable; }
  public void setOutsideTouchable(boolean t) { outsideTouchable = t; }
  public boolean isOutsideTouchable() { return outsideTouchable; }
  public void setBackgroundDrawable(android.graphics.drawable.Drawable d) { background = d; }
  public android.graphics.drawable.Drawable getBackground() { return background; }
  public void setOnDismissListener(OnDismissListener l) { dismissListener = l; }
  public boolean isShowing() { return showing; }
  public void showAsDropDown(android.view.View anchor) { showing = true; }
  public void showAsDropDown(android.view.View anchor, int x, int y) { showing = true; }
  public void showAsDropDown(android.view.View anchor, int x, int y, int gravity) { showing = true; }
  public void showAtLocation(android.view.View parent, int gravity, int x, int y) { showing = true; }
  public void update() {}
  public void update(int x, int y, int w, int h) {}
  public void dismiss() {
    if (!showing) return;
    showing = false;
    if (dismissListener != null) dismissListener.onDismiss();
  }
}