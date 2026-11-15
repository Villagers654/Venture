package android.transition;

import android.content.Context;
import android.graphics.Matrix;
import android.graphics.Rect;
import android.util.AttributeSet;
import android.view.View;
import android.widget.ImageView;

/** AOSP core/java/android/transition/ChangeImageTransform.java: captures an ImageView's image matrix. */
public class ChangeImageTransform extends Transition {
  private static final String PROPNAME_MATRIX = "android:changeImageTransform:matrix";
  private static final String PROPNAME_BOUNDS = "android:changeImageTransform:bounds";
  private static final String[] TRANSITION_PROPERTIES = {PROPNAME_MATRIX, PROPNAME_BOUNDS};

  public ChangeImageTransform() {}
  public ChangeImageTransform(Context context, AttributeSet attrs) { super(context, attrs); }
  @Override public String[] getTransitionProperties() { return TRANSITION_PROPERTIES; }

  private void captureValues(TransitionValues values) {
    View view = values.view;
    if (!(view instanceof ImageView imageView) || view.getVisibility() != View.VISIBLE) return;
    if (imageView.getDrawable() == null) return;
    values.values.put(PROPNAME_BOUNDS, new Rect(view.getLeft(), view.getTop(), view.getRight(), view.getBottom()));
    values.values.put(PROPNAME_MATRIX, new Matrix(imageView.getImageMatrix()));
  }
  @Override public void captureStartValues(TransitionValues transitionValues) { captureValues(transitionValues); }
  @Override public void captureEndValues(TransitionValues transitionValues) { captureValues(transitionValues); }
}
