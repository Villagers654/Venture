package android.bluetooth;
/** Guest adapter with no paired or connected devices. Host radios are not exposed. */
public final class BluetoothAdapter {
  public static final int STATE_OFF = 10, STATE_TURNING_ON = 11, STATE_ON = 12, STATE_TURNING_OFF = 13;
  public static final int SCAN_MODE_NONE = 20, SCAN_MODE_CONNECTABLE = 21, SCAN_MODE_CONNECTABLE_DISCOVERABLE = 23;
  private static final BluetoothAdapter DEFAULT = new BluetoothAdapter(new android.content.Context());
  private final android.content.Context context;
  BluetoothAdapter(android.content.Context context) { this.context = java.util.Objects.requireNonNull(context); }
  public static BluetoothAdapter getDefaultAdapter() { return DEFAULT; }
  static void enforceConnect(android.content.Context context) {
    String permission = android.os.Build.VERSION.SDK_INT >= 31
            && context.getApplicationInfo().targetSdkVersion >= 31
        ? "android.permission.BLUETOOTH_CONNECT" : "android.permission.BLUETOOTH";
    if (context.checkSelfPermission(permission) != android.content.pm.PackageManager.PERMISSION_GRANTED)
      throw new SecurityException("Need " + permission + " permission");
  }
  public int getState() {
    if (android.os.Build.VERSION.SDK_INT < 31 || context.getApplicationInfo().targetSdkVersion < 31)
      enforceConnect(context);
    return STATE_ON;
  }
  public boolean isEnabled() { return getState() == STATE_ON; }
  public java.util.Set<BluetoothDevice> getBondedDevices() {
    enforceConnect(context);
    return java.util.Collections.emptySet();
  }
  public int getProfileConnectionState(int profile) {
    enforceConnect(context);
    return BluetoothProfile.STATE_DISCONNECTED;
  }
}
