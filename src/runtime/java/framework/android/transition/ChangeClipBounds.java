package android.transition;

import android.content.Context;
import android.graphics.Rect;
import android.util.AttributeSet;
import android.view.View;

/** AOSP core/java/android/transition/ChangeClipBounds.java. */
public class ChangeClipBounds extends Transition {
  private static final String PROPNAME_CLIP = "android:clipBounds:clip";
  private static final String PROPNAME_BOUNDS = "android:clipBounds:bounds";
  private static final String[] TRANSITION_PROPERTIES = {PROPNAME_CLIP};

  public ChangeClipBounds() {}
  public ChangeClipBounds(Context context, AttributeSet attrs) { super(context, attrs); }
  @Override public String[] getTransitionProperties() { return TRANSITION_PROPERTIES; }

  // A view here has no clip bounds, which AOSP records as null with the full view bounds.
  private void captureValues(TransitionValues values) {
    View view = values.view;
    if (view.getVisibility() == View.GONE) return;
    values.values.put(PROPNAME_CLIP, null);
    values.values.put(PROPNAME_BOUNDS, new Rect(0, 0, view.getWidth(), view.getHeight()));
  }
  @Override public void captureStartValues(TransitionValues transitionValues) { captureValues(transitionValues); }
  @Override public void captureEndValues(TransitionValues transitionValues) { captureValues(transitionValues); }
}
