package android.view;

/**
 * Android input devices are distinct from controllers supplied by OpenXR. As on any Android device
 * (a Quest included), EventHub's virtual keyboard always exists: id -1, named "Virtual", an
 * alphabetic keyboard source with the virtual key character map and no physical keys. No other
 * Android input-device producer is installed; never invent controller IDs.
 */
public final class InputDevice {
  public static final int SOURCE_KEYBOARD = 0x101, SOURCE_DPAD = 0x201,
      SOURCE_GAMEPAD = 0x401, SOURCE_TOUCHSCREEN = 0x1002, SOURCE_MOUSE = 0x2002,
      SOURCE_JOYSTICK = 0x1000010;
  public static final int KEYBOARD_TYPE_NONE = 0, KEYBOARD_TYPE_NON_ALPHABETIC = 1, KEYBOARD_TYPE_ALPHABETIC = 2;
  // EventHub::createVirtualKeyboardLocked: uniqueId "<virtual>", name "Virtual", no vendor or
  // product; the descriptor is the SHA-1 of ":0000:0000:uniqueId:<virtual>name:Virtual".
  private static final InputDevice VIRTUAL = new InputDevice(KeyCharacterMap.VIRTUAL_KEYBOARD, "Virtual",
      "a718a782d34bc767f4689c232d64d527998ea7fd", SOURCE_KEYBOARD, KEYBOARD_TYPE_ALPHABETIC);
  private static final android.os.Vibrator NO_VIBRATOR = new android.os.Vibrator();

  private final int id, sources, keyboardType;
  private final String name, descriptor;
  private android.os.VibratorManager vibratorManager;
  private InputDevice(int id, String name, String descriptor, int sources, int keyboardType) {
    this.id = id;
    this.name = name;
    this.descriptor = descriptor;
    this.sources = sources;
    this.keyboardType = keyboardType;
  }
  public int getId() { return id; }
  public int getControllerNumber() { return 0; }
  public String getDescriptor() { return descriptor; }
  public String getName() { return name; }
  public int getVendorId() { return 0; }
  public int getProductId() { return 0; }
  public int getSources() { return sources; }
  public boolean supportsSource(int source) { return (sources & source) == source; }
  public int getKeyboardType() { return keyboardType; }
  public KeyCharacterMap getKeyCharacterMap() { return KeyCharacterMap.load(id); }
  public boolean isVirtual() { return id < 0; }
  public boolean isExternal() { return false; }
  public boolean isEnabled() { return true; }
  public boolean hasMicrophone() { return false; }
  /** No key layout backs the virtual keyboard, so it has none of the keys (InputManager.deviceHasKeys). */
  public boolean[] hasKeys(int... keys) { return new boolean[keys.length]; }
  // Devices without a vibrator return a NullVibrator and an empty vibrator manager, never null.
  public android.os.Vibrator getVibrator() { return NO_VIBRATOR; }
  public synchronized android.os.VibratorManager getVibratorManager() {
    if (vibratorManager == null)
      vibratorManager = new android.os.VibratorManager(new android.content.Context()) {
        @Override public android.os.Vibrator getDefaultVibrator() { return NO_VIBRATOR; }
      };
    return vibratorManager;
  }
  public static int[] getDeviceIds() { return new int[] {KeyCharacterMap.VIRTUAL_KEYBOARD}; }
  public static InputDevice getDevice(int id) { return id == KeyCharacterMap.VIRTUAL_KEYBOARD ? VIRTUAL : null; }
}
