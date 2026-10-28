package android.app;
public class NativeActivity extends Activity implements android.view.SurfaceHolder.Callback2 {
  private long nativeActivity;
  protected void onCreate(android.os.Bundle state) {
    // AOSP NativeActivity prepares its detached window before loadNativeCode.
    // Native startup can create worker threads immediately; keep framework
    // construction out of the subclass's remaining startup transaction.
    getWindow().takeSurface(this);
    try {
      var info = getPackageManager().getActivityInfo(getComponentName(), 128);
      String lib = info.metaData.getString("android.app.lib_name", "main"),
             entry = info.metaData.getString("android.app.func_name", "ANativeActivity_onCreate");
      nativeActivity =
          com.venture.runtime.RuntimeEnvironment.host().createNativeActivity(this, lib, entry);
      if (nativeActivity == 0)
        throw new UnsatisfiedLinkError("NativeActivity creation failed");
    } catch (android.content.pm.PackageManager.NameNotFoundException e) {
      throw new IllegalStateException(e);
    }
    super.onCreate(state);
  }
  protected void onStart() {
    super.onStart();
    event(1, 0);
  }
  protected void onResume() {
    super.onResume();
    event(2, 0);
  }
  protected void onPause() {
    event(3, 0);
    super.onPause();
  }
  protected void onStop() {
    event(4, 0);
    super.onStop();
  }
  protected void onDestroy() {
    event(5, 0);
    nativeActivity = 0;
    super.onDestroy();
  }
  public void onWindowFocusChanged(boolean focus) {
    super.onWindowFocusChanged(focus);
    event(focus ? 6 : 7, 0);
  }
  public void onLowMemory() {
    event(8, 0);
  }
  private void event(int event, long surface) {
    if (nativeActivity != 0)
      com.venture.runtime.RuntimeEnvironment.host().nativeActivityEvent(
          nativeActivity, event, surface);
  }
  public void surfaceCreated(android.view.SurfaceHolder h) {
    event(9, h.getSurface().ventureNativeWindow());
  }
  public void surfaceChanged(android.view.SurfaceHolder h, int format, int w, int height) {
    event(10, h.getSurface().ventureNativeWindow());
  }
  public void surfaceDestroyed(android.view.SurfaceHolder h) {
    event(11, h.getSurface().ventureNativeWindow());
  }
  public void surfaceRedrawNeeded(android.view.SurfaceHolder h) {
    event(12, h.getSurface().ventureNativeWindow());
  }
}
