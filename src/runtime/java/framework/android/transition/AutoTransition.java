package android.transition;

import android.content.Context;
import android.util.AttributeSet;

/** AOSP core/java/android/transition/AutoTransition.java: fade out, move and resize, fade in. */
public class AutoTransition extends TransitionSet {
  public AutoTransition() { init(); }
  public AutoTransition(Context context, AttributeSet attrs) {
    super(context, attrs);
    init();
  }
  private void init() {
    setOrdering(ORDERING_SEQUENTIAL);
    addTransition(new Fade(Fade.OUT)).addTransition(new ChangeBounds()).addTransition(new Fade(Fade.IN));
  }
}
