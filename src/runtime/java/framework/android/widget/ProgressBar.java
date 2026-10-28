package android.widget;
public class ProgressBar extends android.view.View {
  private int progress, secondary, max = 100;
  private boolean indeterminate;
  public ProgressBar(android.content.Context c) { super(c); }
  public ProgressBar(android.content.Context c, android.util.AttributeSet a) { super(c, a); }
  public ProgressBar(android.content.Context c, android.util.AttributeSet a, int style) { super(c, a, style); }
  public synchronized void setProgress(int value) { progress = Math.max(0, Math.min(max, value)); invalidate(); }
  public synchronized int getProgress() { return progress; }
  public synchronized void setSecondaryProgress(int value) { secondary = Math.max(0, Math.min(max, value)); invalidate(); }
  public synchronized int getSecondaryProgress() { return secondary; }
  public synchronized void setMax(int value) {
    max = Math.max(0, value); progress = Math.min(progress, max); secondary = Math.min(secondary, max); invalidate();
  }
  public synchronized int getMax() { return max; }
  public synchronized void setIndeterminate(boolean value) { indeterminate = value; invalidate(); }
  public synchronized boolean isIndeterminate() { return indeterminate; }
}
