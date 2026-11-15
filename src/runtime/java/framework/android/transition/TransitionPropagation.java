package android.transition;

import android.view.ViewGroup;

/** AOSP core/java/android/transition/TransitionPropagation.java. */
public abstract class TransitionPropagation {
  public abstract long getStartDelay(ViewGroup sceneRoot, Transition transition,
      TransitionValues startValues, TransitionValues endValues);
  public abstract void captureValues(TransitionValues transitionValues);
  public abstract String[] getPropagationProperties();
}
