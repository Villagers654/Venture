package android.transition;

import android.view.View;

/** AOSP core/java/android/transition/VisibilityPropagation.java: visibility and view centers. */
public abstract class VisibilityPropagation extends TransitionPropagation {
  private static final String PROPNAME_VISIBILITY = "android:visibilityPropagation:visibility";
  private static final String PROPNAME_VIEW_CENTER = "android:visibilityPropagation:center";
  private static final String[] VISIBILITY_PROPAGATION_VALUES = {PROPNAME_VISIBILITY, PROPNAME_VIEW_CENTER};

  @Override public void captureValues(TransitionValues values) {
    View view = values.view;
    Integer visibility = (Integer) values.values.get(Visibility.PROPNAME_VISIBILITY);
    if (visibility == null) visibility = view.getVisibility();
    values.values.put(PROPNAME_VISIBILITY, visibility);
    int[] loc = new int[2];
    view.getLocationOnScreen(loc);
    loc[0] += Math.round(view.getTranslationX());
    loc[0] += view.getWidth() / 2;
    loc[1] += Math.round(view.getTranslationY());
    loc[1] += view.getHeight() / 2;
    values.values.put(PROPNAME_VIEW_CENTER, loc);
  }
  @Override public String[] getPropagationProperties() { return VISIBILITY_PROPAGATION_VALUES; }
  public int getViewVisibility(TransitionValues values) {
    if (values == null) return View.GONE;
    Integer visibility = (Integer) values.values.get(PROPNAME_VISIBILITY);
    return visibility == null ? View.GONE : visibility;
  }
  public int getViewX(TransitionValues values) { return getViewCoordinate(values, 0); }
  public int getViewY(TransitionValues values) { return getViewCoordinate(values, 1); }
  private static int getViewCoordinate(TransitionValues values, int coordinateIndex) {
    if (values == null) return -1;
    int[] coordinates = (int[]) values.values.get(PROPNAME_VIEW_CENTER);
    return coordinates == null ? -1 : coordinates[coordinateIndex];
  }
}
