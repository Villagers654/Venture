package android.widget;
public class SeekBar extends AbsSeekBar {
  public interface OnSeekBarChangeListener {
    void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser);
    void onStartTrackingTouch(SeekBar seekBar);
    void onStopTrackingTouch(SeekBar seekBar);
  }
  private OnSeekBarChangeListener listener;
  public SeekBar(android.content.Context c) { super(c); }
  public SeekBar(android.content.Context c, android.util.AttributeSet a) { super(c, a); }
  public SeekBar(android.content.Context c, android.util.AttributeSet a, int style) { super(c, a, style); }
  public SeekBar(android.content.Context c, android.util.AttributeSet a, int style, int res) { super(c, a, style, res); }
  public void setOnSeekBarChangeListener(OnSeekBarChangeListener l) { listener = l; }
  @Override public synchronized void setProgress(int value) {
    int before = getProgress();
    super.setProgress(value);
    if (listener != null && getProgress() != before) listener.onProgressChanged(this, getProgress(), false);
  }
}