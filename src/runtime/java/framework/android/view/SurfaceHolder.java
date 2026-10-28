package android.view;
public interface SurfaceHolder {
  void addCallback(Callback c);
  void removeCallback(Callback c);
  Surface getSurface();
  android.graphics.Rect getSurfaceFrame();
  void setFixedSize(int width, int height);
  void setSizeFromLayout();
  void setFormat(int format);
  void setType(int type);
  void setKeepScreenOn(boolean keep);
  boolean isCreating();
  interface Callback {
    void surfaceCreated(SurfaceHolder holder);
    void surfaceChanged(SurfaceHolder holder, int format, int width, int height);
    void surfaceDestroyed(SurfaceHolder holder);
  }
  interface Callback2 extends Callback {
    void surfaceRedrawNeeded(SurfaceHolder holder);
    default void surfaceRedrawNeededAsync(SurfaceHolder holder, Runnable drawingFinished) {
      surfaceRedrawNeeded(holder);
      drawingFinished.run();
    }
  }
}
