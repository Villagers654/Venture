package android.transition;

import android.content.Context;
import android.graphics.Rect;
import android.util.AttributeSet;
import android.view.View;

/** AOSP core/java/android/transition/ChangeBounds.java: captures layout bounds and parents. */
public class ChangeBounds extends Transition {
  private static final String PROPNAME_BOUNDS = "android:changeBounds:bounds";
  private static final String PROPNAME_CLIP = "android:changeBounds:clip";
  private static final String PROPNAME_PARENT = "android:changeBounds:parent";
  private static final String PROPNAME_WINDOW_X = "android:changeBounds:windowX";
  private static final String PROPNAME_WINDOW_Y = "android:changeBounds:windowY";
  private static final String[] TRANSITION_PROPERTIES =
      {PROPNAME_BOUNDS, PROPNAME_CLIP, PROPNAME_PARENT, PROPNAME_WINDOW_X, PROPNAME_WINDOW_Y};
  private boolean resizeClip, reparent;

  public ChangeBounds() {}
  public ChangeBounds(Context context, AttributeSet attrs) { super(context, attrs); }
  @Override public String[] getTransitionProperties() { return TRANSITION_PROPERTIES; }
  public void setResizeClip(boolean resizeClip) { this.resizeClip = resizeClip; }
  public boolean getResizeClip() { return resizeClip; }
  @Deprecated public void setReparent(boolean reparent) { this.reparent = reparent; }

  private void captureValues(TransitionValues values) {
    View view = values.view;
    if (view.isLaidOut() || view.getWidth() != 0 || view.getHeight() != 0) {
      values.values.put(PROPNAME_BOUNDS, new Rect(view.getLeft(), view.getTop(), view.getRight(), view.getBottom()));
      values.values.put(PROPNAME_PARENT, view.getParent());
      if (reparent) {
        int[] location = new int[2];
        view.getLocationInWindow(location);
        values.values.put(PROPNAME_WINDOW_X, location[0]);
        values.values.put(PROPNAME_WINDOW_Y, location[1]);
      }
      // A view here has no clip bounds, which AOSP records as null.
      if (resizeClip) values.values.put(PROPNAME_CLIP, null);
    }
  }
  @Override public void captureStartValues(TransitionValues transitionValues) { captureValues(transitionValues); }
  @Override public void captureEndValues(TransitionValues transitionValues) { captureValues(transitionValues); }
}
