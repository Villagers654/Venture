package android.widget;
public class RatingBar extends AbsSeekBar {
  public interface OnRatingBarChangeListener {
    void onRatingChanged(RatingBar ratingBar, float rating, boolean fromUser);
  }
  private int stars = 5;
  private float step = 0.5f;
  private boolean indicator;
  private OnRatingBarChangeListener listener;
  public RatingBar(android.content.Context c) { super(c); }
  public RatingBar(android.content.Context c, android.util.AttributeSet a) { super(c, a); }
  public RatingBar(android.content.Context c, android.util.AttributeSet a, int style) { super(c, a, style); }
  public RatingBar(android.content.Context c, android.util.AttributeSet a, int style, int res) { super(c, a, style, res); }
  public void setNumStars(int n) { if (n > 0) stars = n; }
  public int getNumStars() { return stars; }
  public void setStepSize(float s) { if (s > 0) step = s; }
  public float getStepSize() { return step; }
  public void setIsIndicator(boolean value) { indicator = value; }
  public boolean isIndicator() { return indicator; }
  public void setOnRatingBarChangeListener(OnRatingBarChangeListener l) { listener = l; }
  public OnRatingBarChangeListener getOnRatingBarChangeListener() { return listener; }
  // AOSP: progress counts steps; max = stars / step.
  public void setRating(float rating) {
    setMax(Math.round(stars / step));
    setProgress(Math.round(rating / step));
    if (listener != null) listener.onRatingChanged(this, getRating(), false);
  }
  public float getRating() { return getProgress() * step; }
}