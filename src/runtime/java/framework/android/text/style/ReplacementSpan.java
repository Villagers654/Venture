package android.text.style;
// Emoji and icon spans (androidx.emoji2) extend this; no host view draws them.
public abstract class ReplacementSpan extends MetricAffectingSpan {
  public abstract int getSize(android.graphics.Paint paint, CharSequence text, int start, int end,
                              android.graphics.Paint.FontMetricsInt metrics);
  public abstract void draw(android.graphics.Canvas canvas, CharSequence text, int start, int end, float x,
                            int top, int y, int bottom, android.graphics.Paint paint);
  public CharSequence getContentDescription() {
    return null;
  }
  public void setContentDescription(CharSequence description) {}
  @Override
  public void updateMeasureState(android.text.TextPaint paint) {}
  @Override
  public void updateDrawState(android.text.TextPaint paint) {}
}
