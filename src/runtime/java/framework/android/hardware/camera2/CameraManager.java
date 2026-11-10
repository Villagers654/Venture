package android.hardware.camera2;
// The camera service of a device without cameras: Venture exposes no headset camera to games,
// so the ID lists are empty and no availability or torch event ever fires. Unity's player
// registers an AvailabilityCallback at start (Ballrs). Opening or querying a camera is left
// out, as no listed ID exists to pass to it.
public final class CameraManager {
  public abstract static class AvailabilityCallback {
    public void onCameraAvailable(String cameraId) {}
    public void onCameraUnavailable(String cameraId) {}
    public void onCameraAccessPrioritiesChanged() {}
    public void onPhysicalCameraAvailable(String cameraId, String physicalCameraId) {}
    public void onPhysicalCameraUnavailable(String cameraId, String physicalCameraId) {}
  }
  public abstract static class TorchCallback {
    public void onTorchModeUnavailable(String cameraId) {}
    public void onTorchModeChanged(String cameraId, boolean enabled) {}
    public void onTorchStrengthLevelChanged(String cameraId, int newStrengthLevel) {}
  }
  public String[] getCameraIdList() throws CameraAccessException { return new String[0]; }
  public java.util.Set<java.util.Set<String>> getConcurrentCameraIds() throws CameraAccessException {
    return java.util.Set.of();
  }
  public void registerAvailabilityCallback(AvailabilityCallback callback, android.os.Handler handler) {
    java.util.Objects.requireNonNull(callback, "callback");
  }
  public void registerAvailabilityCallback(java.util.concurrent.Executor executor, AvailabilityCallback callback) {
    java.util.Objects.requireNonNull(executor, "executor");
    java.util.Objects.requireNonNull(callback, "callback");
  }
  public void unregisterAvailabilityCallback(AvailabilityCallback callback) {}
  public void registerTorchCallback(TorchCallback callback, android.os.Handler handler) {
    java.util.Objects.requireNonNull(callback, "callback");
  }
  public void registerTorchCallback(java.util.concurrent.Executor executor, TorchCallback callback) {
    java.util.Objects.requireNonNull(executor, "executor");
    java.util.Objects.requireNonNull(callback, "callback");
  }
  public void unregisterTorchCallback(TorchCallback callback) {}
}
