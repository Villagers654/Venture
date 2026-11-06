package android.app;

/** Progress state; like every Dialog it is reported shown but not presented. */
public class ProgressDialog extends AlertDialog {
  public static final int STYLE_SPINNER = 0, STYLE_HORIZONTAL = 1;
  private int progress, secondaryProgress, max = 100, style = STYLE_SPINNER;
  private boolean indeterminate;
  public ProgressDialog(android.content.Context context) { super(context); }
  public void setProgressStyle(int value) { style = value; }
  public void setMax(int value) {
    max = Math.max(0, value); setProgress(progress); setSecondaryProgress(secondaryProgress);
  }
  public int getMax() { return max; }
  public void setProgress(int value) { progress = Math.max(0, Math.min(max, value)); }
  public int getProgress() { return progress; }
  public void setSecondaryProgress(int value) { secondaryProgress = Math.max(0, Math.min(max, value)); }
  public int getSecondaryProgress() { return secondaryProgress; }
  public void incrementProgressBy(int value) { setProgress(progress + value); }
  public void incrementSecondaryProgressBy(int value) { setSecondaryProgress(secondaryProgress + value); }
  public void setIndeterminate(boolean value) { indeterminate = value; }
  public boolean isIndeterminate() { return indeterminate; }
}
