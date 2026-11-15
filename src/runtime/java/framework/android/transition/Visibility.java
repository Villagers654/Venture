package android.transition;

import android.animation.Animator;
import android.content.Context;
import android.util.AttributeSet;
import android.view.View;
import android.view.ViewGroup;

/** AOSP core/java/android/transition/Visibility.java: appear and disappear transitions. */
public abstract class Visibility extends Transition {
  static final String PROPNAME_VISIBILITY = "android:visibility:visibility";
  private static final String PROPNAME_PARENT = "android:visibility:parent";
  static final String PROPNAME_SCREEN_LOCATION = "android:visibility:screenLocation";
  public static final int MODE_IN = 0x1, MODE_OUT = 0x2;
  private static final String[] TRANSITION_PROPERTIES = {PROPNAME_VISIBILITY, PROPNAME_PARENT};
  private int mode = MODE_IN | MODE_OUT;

  public Visibility() {}
  public Visibility(Context context, AttributeSet attrs) { super(context, attrs); }

  public void setMode(int mode) {
    if ((mode & ~(MODE_IN | MODE_OUT)) != 0) throw new IllegalArgumentException("Only MODE_IN and MODE_OUT flags are allowed");
    this.mode = mode;
  }
  public int getMode() { return mode; }
  @Override public String[] getTransitionProperties() { return TRANSITION_PROPERTIES; }

  private void captureValues(TransitionValues transitionValues) {
    transitionValues.values.put(PROPNAME_VISIBILITY, transitionValues.view.getVisibility());
    transitionValues.values.put(PROPNAME_PARENT, transitionValues.view.getParent());
    int[] loc = new int[2];
    transitionValues.view.getLocationOnScreen(loc);
    transitionValues.values.put(PROPNAME_SCREEN_LOCATION, loc);
  }
  @Override public void captureStartValues(TransitionValues transitionValues) { captureValues(transitionValues); }
  @Override public void captureEndValues(TransitionValues transitionValues) { captureValues(transitionValues); }

  public boolean isVisible(TransitionValues values) {
    if (values == null) return false;
    Integer visibility = (Integer) values.values.get(PROPNAME_VISIBILITY);
    return visibility != null && visibility == View.VISIBLE && values.values.get(PROPNAME_PARENT) != null;
  }

  private static final class VisibilityInfo {
    boolean visibilityChange, fadeIn;
    int startVisibility, endVisibility;
    Object startParent, endParent;
  }
  private static VisibilityInfo getVisibilityChangeInfo(TransitionValues startValues, TransitionValues endValues) {
    var info = new VisibilityInfo();
    if (startValues != null && startValues.values.containsKey(PROPNAME_VISIBILITY)) {
      info.startVisibility = (Integer) startValues.values.get(PROPNAME_VISIBILITY);
      info.startParent = startValues.values.get(PROPNAME_PARENT);
    } else {
      info.startVisibility = -1;
    }
    if (endValues != null && endValues.values.containsKey(PROPNAME_VISIBILITY)) {
      info.endVisibility = (Integer) endValues.values.get(PROPNAME_VISIBILITY);
      info.endParent = endValues.values.get(PROPNAME_PARENT);
    } else {
      info.endVisibility = -1;
    }
    if (startValues != null && endValues != null) {
      if (info.startVisibility == info.endVisibility && info.startParent == info.endParent) return info;
      if (info.startVisibility != info.endVisibility) {
        if (info.startVisibility == View.VISIBLE) {
          info.fadeIn = false;
          info.visibilityChange = true;
        } else if (info.endVisibility == View.VISIBLE) {
          info.fadeIn = true;
          info.visibilityChange = true;
        }
      } else if (info.endParent == null) {
        info.fadeIn = false;
        info.visibilityChange = true;
      } else if (info.startParent == null) {
        info.fadeIn = true;
        info.visibilityChange = true;
      }
    } else if (startValues == null && info.endVisibility == View.VISIBLE) {
      info.fadeIn = true;
      info.visibilityChange = true;
    } else if (endValues == null && info.startVisibility == View.VISIBLE) {
      info.fadeIn = false;
      info.visibilityChange = true;
    }
    return info;
  }

  @Override public Animator createAnimator(ViewGroup sceneRoot, TransitionValues startValues, TransitionValues endValues) {
    VisibilityInfo info = getVisibilityChangeInfo(startValues, endValues);
    if (info.visibilityChange && (info.startParent != null || info.endParent != null)) {
      return info.fadeIn
          ? onAppear(sceneRoot, startValues, info.startVisibility, endValues, info.endVisibility)
          : onDisappear(sceneRoot, startValues, info.startVisibility, endValues, info.endVisibility);
    }
    return null;
  }
  public Animator onAppear(ViewGroup sceneRoot, TransitionValues startValues, int startVisibility,
      TransitionValues endValues, int endVisibility) {
    if ((mode & MODE_IN) != MODE_IN || endValues == null) return null;
    return onAppear(sceneRoot, endValues.view, startValues, endValues);
  }
  public Animator onAppear(ViewGroup sceneRoot, View view, TransitionValues startValues, TransitionValues endValues) {
    return null;
  }
  public Animator onDisappear(ViewGroup sceneRoot, TransitionValues startValues, int startVisibility,
      TransitionValues endValues, int endVisibility) {
    if ((mode & MODE_OUT) != MODE_OUT || startValues == null) return null;
    return onDisappear(sceneRoot, startValues.view, startValues, endValues);
  }
  public Animator onDisappear(ViewGroup sceneRoot, View view, TransitionValues startValues, TransitionValues endValues) {
    return null;
  }

  @Override public boolean isTransitionRequired(TransitionValues startValues, TransitionValues newValues) {
    if (startValues == null && newValues == null) return false;
    if (startValues != null && newValues != null
        && newValues.values.containsKey(PROPNAME_VISIBILITY) != startValues.values.containsKey(PROPNAME_VISIBILITY))
      return false;
    VisibilityInfo info = getVisibilityChangeInfo(startValues, newValues);
    return info.visibilityChange && (info.startVisibility == View.VISIBLE || info.endVisibility == View.VISIBLE);
  }
}
