package android.transition;

import android.content.Context;
import android.util.AttributeSet;
import android.view.View;

/** AOSP core/java/android/transition/ChangeTransform.java: captures view transforms and parents. */
public class ChangeTransform extends Transition {
  private static final String PROPNAME_MATRIX = "android:changeTransform:matrix";
  private static final String PROPNAME_TRANSFORMS = "android:changeTransform:transforms";
  private static final String PROPNAME_PARENT = "android:changeTransform:parent";
  private static final String PROPNAME_PARENT_MATRIX = "android:changeTransform:parentMatrix";
  private static final String PROPNAME_INTERMEDIATE_PARENT_MATRIX = "android:changeTransform:intermediateParentMatrix";
  private static final String PROPNAME_INTERMEDIATE_MATRIX = "android:changeTransform:intermediateMatrix";
  private static final String[] TRANSITION_PROPERTIES = {PROPNAME_MATRIX, PROPNAME_TRANSFORMS, PROPNAME_PARENT_MATRIX};
  private boolean useOverlay = true, reparent = true;

  public ChangeTransform() {}
  public ChangeTransform(Context context, AttributeSet attrs) { super(context, attrs); }
  @Override public String[] getTransitionProperties() { return TRANSITION_PROPERTIES; }
  public boolean getReparentWithOverlay() { return useOverlay; }
  public void setReparentWithOverlay(boolean reparentWithOverlay) { useOverlay = reparentWithOverlay; }
  public boolean getReparent() { return reparent; }
  public void setReparent(boolean reparent) { this.reparent = reparent; }

  // The view transform here is its translation; views carry no scale or rotation.
  private void captureValues(TransitionValues values) {
    View view = values.view;
    if (view.getVisibility() == View.GONE) return;
    values.values.put(PROPNAME_PARENT, view.getParent());
    values.values.put(PROPNAME_TRANSFORMS, new float[] {view.getTranslationX(), view.getTranslationY()});
    var matrix = new android.graphics.Matrix();
    matrix.setTranslate(view.getTranslationX(), view.getTranslationY());
    values.values.put(PROPNAME_MATRIX, matrix);
  }
  @Override public void captureStartValues(TransitionValues transitionValues) { captureValues(transitionValues); }
  @Override public void captureEndValues(TransitionValues transitionValues) { captureValues(transitionValues); }
}
