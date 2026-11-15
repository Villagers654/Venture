package android.transition;

import android.content.Context;
import android.util.AttributeSet;

/** AOSP core/java/android/transition/Explode.java: views move from or to the epicenter. */
public class Explode extends Visibility {
  public Explode() { setPropagation(new CircularPropagation()); }
  public Explode(Context context, AttributeSet attrs) {
    super(context, attrs);
    setPropagation(new CircularPropagation());
  }
}
