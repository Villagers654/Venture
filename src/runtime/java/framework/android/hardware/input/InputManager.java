package android.hardware.input;

public final class InputManager {
  public interface InputDeviceListener {
    void onInputDeviceAdded(int deviceId);
    void onInputDeviceRemoved(int deviceId);
    void onInputDeviceChanged(int deviceId);
  }
  private static final InputManager INSTANCE = new InputManager();
  private final java.util.Map<InputDeviceListener, android.os.Handler> listeners = new java.util.HashMap<>();
  public static InputManager getInstance() { return INSTANCE; }
  public int[] getInputDeviceIds() { return android.view.InputDevice.getDeviceIds(); }
  public android.view.InputDevice getInputDevice(int id) { return android.view.InputDevice.getDevice(id); }
  public synchronized void registerInputDeviceListener(InputDeviceListener listener, android.os.Handler handler) {
    java.util.Objects.requireNonNull(listener);
    listeners.put(listener, handler == null ? new android.os.Handler() : handler);
    android.util.Log.w("InputManager", "Android input-device producer unavailable; XR controllers use OpenXR");
  }
  public synchronized void unregisterInputDeviceListener(InputDeviceListener listener) { listeners.remove(listener); }
}
