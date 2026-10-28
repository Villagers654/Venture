package android.bluetooth;
public interface BluetoothProfile {
  int STATE_DISCONNECTED = 0, STATE_CONNECTING = 1, STATE_CONNECTED = 2, STATE_DISCONNECTING = 3;
  int HEADSET = 1, A2DP = 2, GATT = 7, GATT_SERVER = 8;
}
