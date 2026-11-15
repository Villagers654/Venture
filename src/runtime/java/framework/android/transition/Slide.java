package android.transition;

import android.content.Context;
import android.util.AttributeSet;
import android.view.Gravity;

/** AOSP core/java/android/transition/Slide.java: slides views in and out at one edge. */
public class Slide extends Visibility {
  private int slideEdge = Gravity.BOTTOM;
  public Slide() { setSlideEdge(Gravity.BOTTOM); }
  public Slide(int slideEdge) { setSlideEdge(slideEdge); }
  public Slide(Context context, AttributeSet attrs) {
    super(context, attrs);
    setSlideEdge(Gravity.BOTTOM);
  }
  public void setSlideEdge(int slideEdge) {
    switch (slideEdge) {
      case Gravity.LEFT, Gravity.TOP, Gravity.RIGHT, Gravity.BOTTOM, Gravity.START, Gravity.END -> {}
      default -> throw new IllegalArgumentException("Invalid slide direction");
    }
    this.slideEdge = slideEdge;
    SidePropagation propagation = new SidePropagation();
    propagation.setSide(slideEdge);
    setPropagation(propagation);
  }
  public int getSlideEdge() { return slideEdge; }
}
