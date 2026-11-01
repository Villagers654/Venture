package android.view;
public abstract class ViewOutlineProvider {
  public static final ViewOutlineProvider BACKGROUND = new ViewOutlineProvider() {
    public void getOutline(View view, android.graphics.Outline outline) {
      outline.setRect(0, 0, view.getWidth(), view.getHeight());
      outline.setAlpha(0);
    }
  };
  public static final ViewOutlineProvider BOUNDS = new ViewOutlineProvider() {
    public void getOutline(View view, android.graphics.Outline outline) {
      outline.setRect(0, 0, view.getWidth(), view.getHeight());
    }
  };
  public static final ViewOutlineProvider PADDED_BOUNDS = BOUNDS;
  public abstract void getOutline(View view, android.graphics.Outline outline);
}
