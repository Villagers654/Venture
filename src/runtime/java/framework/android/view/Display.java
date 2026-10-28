package android.view;
public class Display {
  public static final int DEFAULT_DISPLAY = 0;
  public static final class Mode {
    private final int width, height;
    private final float refreshRate;
    Mode(int w, int h, float rate) { width = w; height = h; refreshRate = rate; }
    public int getModeId() { return 1; }
    public int getPhysicalWidth() { return width; }
    public int getPhysicalHeight() { return height; }
    public float getRefreshRate() { return refreshRate; }
  }
  public Mode getMode() { return new Mode(getWidth(), getHeight(), getRefreshRate()); }
  public Mode[] getSupportedModes() { return new Mode[]{getMode()}; }
  public boolean isWideColorGamut() { return false; }
  public boolean isHdr() { return false; }
  // Framework callbacks use the XR display timestamp without an early phase.
  public long getAppVsyncOffsetNanos() { return 0; }
  public long getPresentationDeadlineNanos() { return Math.round(1_000_000_000d / getRefreshRate()); }
  public int getDisplayId() {
    return 0;
  }
  public int getRotation() {
    return 0;
  }
  public int getOrientation() {
    return 0;
  }
  public int getWidth() {
    return com.venture.runtime.RuntimeEnvironment.resources().getDisplayMetrics().widthPixels;
  }
  public int getHeight() {
    return com.venture.runtime.RuntimeEnvironment.resources().getDisplayMetrics().heightPixels;
  }
  public void getMetrics(android.util.DisplayMetrics out) {
    out.setTo(com.venture.runtime.RuntimeEnvironment.resources().getDisplayMetrics());
  }
  public void getRealMetrics(android.util.DisplayMetrics out) {
    getMetrics(out);
  }
  public void getSize(android.graphics.Point p) {
    p.set(getWidth(), getHeight());
  }
  public void getRealSize(android.graphics.Point p) {
    getSize(p);
  }
  public float getRefreshRate() {
    return com.venture.runtime.RuntimeEnvironment.get().refreshRate;
  }
}
