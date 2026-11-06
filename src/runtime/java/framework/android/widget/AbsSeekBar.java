package android.widget;
public abstract class AbsSeekBar extends ProgressBar {
  private int keyProgressIncrement = 1;
  public AbsSeekBar(android.content.Context c) { super(c); }
  public AbsSeekBar(android.content.Context c, android.util.AttributeSet a) { super(c, a); }
  public AbsSeekBar(android.content.Context c, android.util.AttributeSet a, int style) { super(c, a, style); }
  public AbsSeekBar(android.content.Context c, android.util.AttributeSet a, int style, int res) { super(c, a, style); }
  public void setKeyProgressIncrement(int increment) { keyProgressIncrement = Math.abs(increment); }
  public int getKeyProgressIncrement() { return keyProgressIncrement; }
}