package android.transition;

import android.content.Context;
import android.util.AttributeSet;

/** AOSP core/java/android/transition/ChangeScroll.java: captures scroll offsets. */
public class ChangeScroll extends Transition {
  private static final String PROPNAME_SCROLL_X = "android:changeScroll:x";
  private static final String PROPNAME_SCROLL_Y = "android:changeScroll:y";
  private static final String[] PROPERTIES = {PROPNAME_SCROLL_X, PROPNAME_SCROLL_Y};

  public ChangeScroll() {}
  public ChangeScroll(Context context, AttributeSet attrs) { super(context, attrs); }
  @Override public String[] getTransitionProperties() { return PROPERTIES; }

  private void captureValues(TransitionValues values) {
    values.values.put(PROPNAME_SCROLL_X, values.view.getScrollX());
    values.values.put(PROPNAME_SCROLL_Y, values.view.getScrollY());
  }
  @Override public void captureStartValues(TransitionValues transitionValues) { captureValues(transitionValues); }
  @Override public void captureEndValues(TransitionValues transitionValues) { captureValues(transitionValues); }
}
