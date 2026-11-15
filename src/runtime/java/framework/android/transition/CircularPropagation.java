package android.transition;

import android.graphics.Rect;
import android.view.View;
import android.view.ViewGroup;

/** AOSP core/java/android/transition/CircularPropagation.java: delay grows with distance from the epicenter. */
public class CircularPropagation extends VisibilityPropagation {
  private float propagationSpeed = 3.0f;

  public void setPropagationSpeed(float propagationSpeed) {
    if (propagationSpeed == 0) throw new IllegalArgumentException("propagationSpeed may not be 0");
    this.propagationSpeed = propagationSpeed;
  }

  @Override public long getStartDelay(ViewGroup sceneRoot, Transition transition,
      TransitionValues startValues, TransitionValues endValues) {
    if (startValues == null && endValues == null) return 0;
    int directionMultiplier = 1;
    TransitionValues positionValues;
    if (endValues == null || getViewVisibility(startValues) == View.VISIBLE) {
      positionValues = startValues;
      directionMultiplier = -1;
    } else {
      positionValues = endValues;
    }
    int viewCenterX = getViewX(positionValues), viewCenterY = getViewY(positionValues);
    Rect epicenter = transition.getEpicenter();
    int epicenterX, epicenterY;
    if (epicenter != null) {
      epicenterX = (epicenter.left + epicenter.right) >> 1;
      epicenterY = (epicenter.top + epicenter.bottom) >> 1;
    } else {
      int[] loc = new int[2];
      sceneRoot.getLocationOnScreen(loc);
      epicenterX = Math.round(loc[0] + (sceneRoot.getWidth() / 2) + sceneRoot.getTranslationX());
      epicenterY = Math.round(loc[1] + (sceneRoot.getHeight() / 2) + sceneRoot.getTranslationY());
    }
    double distance = distance(viewCenterX, viewCenterY, epicenterX, epicenterY);
    double maxDistance = distance(0, 0, sceneRoot.getWidth(), sceneRoot.getHeight());
    double distanceFraction = distance / maxDistance;
    long duration = transition.getDuration();
    if (duration < 0) duration = 300;
    return Math.round(duration * directionMultiplier / propagationSpeed * distanceFraction);
  }
  private static double distance(float x1, float y1, float x2, float y2) {
    double x = x2 - x1, y = y2 - y1;
    return Math.hypot(x, y);
  }
}
