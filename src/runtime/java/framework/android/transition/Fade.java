package android.transition;

import android.content.Context;
import android.util.AttributeSet;

/** AOSP core/java/android/transition/Fade.java. */
public class Fade extends Visibility {
  public static final int IN = Visibility.MODE_IN, OUT = Visibility.MODE_OUT;
  public Fade() {}
  public Fade(int fadingMode) { setMode(fadingMode); }
  public Fade(Context context, AttributeSet attrs) { super(context, attrs); }
}
