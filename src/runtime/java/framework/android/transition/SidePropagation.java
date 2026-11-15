package android.transition;

import android.graphics.Rect;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;

/** AOSP core/java/android/transition/SidePropagation.java: delay grows with distance from one edge. */
public class SidePropagation extends VisibilityPropagation {
  private float propagationSpeed = 3.0f;
  private int side = Gravity.BOTTOM;

  public void setSide(int side) { this.side = side; }
  public void setPropagationSpeed(float propagationSpeed) {
    if (propagationSpeed == 0) throw new IllegalArgumentException("propagationSpeed may not be 0");
    this.propagationSpeed = propagationSpeed;
  }

  @Override public long getStartDelay(ViewGroup sceneRoot, Transition transition,
      TransitionValues startValues, TransitionValues endValues) {
    if (startValues == null && endValues == null) return 0;
    int directionMultiplier = 1;
    Rect epicenter = transition.getEpicenter();
    TransitionValues positionValues;
    if (endValues == null || getViewVisibility(startValues) == View.VISIBLE) {
      positionValues = startValues;
      directionMultiplier = -1;
    } else {
      positionValues = endValues;
    }
    int viewCenterX = getViewX(positionValues), viewCenterY = getViewY(positionValues);
    int[] loc = new int[2];
    sceneRoot.getLocationOnScreen(loc);
    int left = loc[0] + Math.round(sceneRoot.getTranslationX());
    int top = loc[1] + Math.round(sceneRoot.getTranslationY());
    int right = left + sceneRoot.getWidth(), bottom = top + sceneRoot.getHeight();
    int epicenterX, epicenterY;
    if (epicenter != null) {
      epicenterX = (epicenter.left + epicenter.right) >> 1;
      epicenterY = (epicenter.top + epicenter.bottom) >> 1;
    } else {
      epicenterX = (left + right) / 2;
      epicenterY = (top + bottom) / 2;
    }
    float distance = distance(viewCenterX, viewCenterY, epicenterX, epicenterY, left, top, right, bottom);
    float maxDistance = getMaxDistance(sceneRoot);
    float distanceFraction = distance / maxDistance;
    long duration = transition.getDuration();
    if (duration < 0) duration = 300;
    return Math.round(duration * directionMultiplier / propagationSpeed * distanceFraction);
  }

  // Venture's views lay out left to right, so START is LEFT and END is RIGHT.
  private int resolvedSide() {
    return side == Gravity.START ? Gravity.LEFT : side == Gravity.END ? Gravity.RIGHT : side;
  }
  private int distance(int viewX, int viewY, int epicenterX, int epicenterY, int left, int top, int right, int bottom) {
    return switch (resolvedSide()) {
      case Gravity.LEFT -> right - viewX + Math.abs(epicenterY - viewY);
      case Gravity.TOP -> bottom - viewY + Math.abs(epicenterX - viewX);
      case Gravity.RIGHT -> viewX - left + Math.abs(epicenterY - viewY);
      case Gravity.BOTTOM -> viewY - top + Math.abs(epicenterX - viewX);
      default -> 0;
    };
  }
  private int getMaxDistance(ViewGroup sceneRoot) {
    return switch (side) {
      case Gravity.LEFT, Gravity.RIGHT, Gravity.START, Gravity.END -> sceneRoot.getWidth();
      default -> sceneRoot.getHeight();
    };
  }
}
