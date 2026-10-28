package android.view;

/** Android input devices are distinct from controllers supplied by OpenXR. */
public final class InputDevice {
  public static final int SOURCE_KEYBOARD = 0x101, SOURCE_DPAD = 0x201,
      SOURCE_GAMEPAD = 0x401, SOURCE_TOUCHSCREEN = 0x1002, SOURCE_MOUSE = 0x2002,
      SOURCE_JOYSTICK = 0x1000010;
  private InputDevice() {}
  // No Android input-device producer is installed. Never invent controller IDs.
  public static int[] getDeviceIds() { return new int[0]; }
  public static InputDevice getDevice(int id) { return null; }
}
