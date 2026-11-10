package android.hardware.camera2;
public class CameraAccessException extends android.util.AndroidException {
  public static final int CAMERA_IN_USE = 4, MAX_CAMERAS_IN_USE = 5, CAMERA_DISABLED = 1,
      CAMERA_DISCONNECTED = 2, CAMERA_ERROR = 3;
  private final int reason;
  public CameraAccessException(int problem) { this(problem, null, null); }
  public CameraAccessException(int problem, String message) { this(problem, message, null); }
  public CameraAccessException(int problem, String message, Throwable cause) {
    super(message, cause);
    reason = problem;
  }
  public CameraAccessException(int problem, Throwable cause) { this(problem, null, cause); }
  public final int getReason() { return reason; }
}
