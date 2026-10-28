package android.bluetooth;

/** Guest Bluetooth service with an enabled adapter and no connected devices. */
public final class BluetoothManager {
  private final android.content.Context context;
  private final BluetoothAdapter adapter;
  public BluetoothManager() { this(new android.content.Context()); }
  public BluetoothManager(android.content.Context context) {
    this.context = java.util.Objects.requireNonNull(context);
    adapter = new BluetoothAdapter(context);
  }
  public BluetoothAdapter getAdapter() { return adapter; }
  private static void checkProfile(int profile) {
    if (profile != BluetoothProfile.GATT && profile != BluetoothProfile.GATT_SERVER)
      throw new IllegalArgumentException("Profile must be GATT or GATT_SERVER");
  }
  public java.util.List<BluetoothDevice> getConnectedDevices(int profile) {
    checkProfile(profile);
    BluetoothAdapter.enforceConnect(context);
    return new java.util.ArrayList<>();
  }
  public java.util.List<BluetoothDevice> getDevicesMatchingConnectionStates(int profile, int[] states) {
    checkProfile(profile);
    BluetoothAdapter.enforceConnect(context);
    return new java.util.ArrayList<>();
  }
  public int getConnectionState(BluetoothDevice device, int profile) {
    checkProfile(profile);
    BluetoothAdapter.enforceConnect(context);
    return BluetoothProfile.STATE_DISCONNECTED;
  }
}
