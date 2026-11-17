package android.hardware;

/** The deprecated camera API sees no cameras, matching camera2's empty camera list. */
@Deprecated
public class Camera {
  public static class CameraInfo {
    public static final int CAMERA_FACING_BACK = 0, CAMERA_FACING_FRONT = 1;
    public int facing, orientation;
    public boolean canDisableShutterSound;
  }
  public static int getNumberOfCameras() { return 0; }
  public static void getCameraInfo(int cameraId, CameraInfo info) {
    throw new RuntimeException("Unknown camera ID");
  }
  public static Camera open(int cameraId) { throw new RuntimeException("Fail to connect to camera service"); }
  public static Camera open() { return null; }
}
