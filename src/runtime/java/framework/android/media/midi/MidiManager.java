package android.media.midi;

/**
 * Context.MIDI_SERVICE. Venture exposes no Android MIDI devices (USB, Bluetooth
 * or virtual) and PackageManager does not report FEATURE_MIDI, so the device
 * list is empty and registered callbacks are kept but never called. A device
 * info can only come from this service, so opening one is refused as the
 * service refuses an unknown device.
 */
public final class MidiManager {
  public static final int TRANSPORT_MIDI_BYTE_STREAM = 1, TRANSPORT_UNIVERSAL_MIDI_PACKETS = 2;
  public static class DeviceCallback {
    public void onDeviceAdded(MidiDeviceInfo device) {}
    public void onDeviceRemoved(MidiDeviceInfo device) {}
    public void onDeviceStatusChanged(MidiDeviceStatus status) {}
  }
  public interface OnDeviceOpenedListener {
    void onDeviceOpened(MidiDevice device);
  }
  private final java.util.Set<DeviceCallback> callbacks = java.util.concurrent.ConcurrentHashMap.newKeySet();
  public void registerDeviceCallback(DeviceCallback callback, android.os.Handler handler) {
    callbacks.add(java.util.Objects.requireNonNull(callback));
  }
  public void registerDeviceCallback(int transport, java.util.concurrent.Executor executor, DeviceCallback callback) {
    java.util.Objects.requireNonNull(executor);
    checkTransport(transport);
    callbacks.add(java.util.Objects.requireNonNull(callback));
  }
  public void unregisterDeviceCallback(DeviceCallback callback) { callbacks.remove(callback); }
  @Deprecated
  public MidiDeviceInfo[] getDevices() { return new MidiDeviceInfo[0]; }
  public java.util.Set<MidiDeviceInfo> getDevicesForTransport(int transport) {
    checkTransport(transport);
    return new java.util.HashSet<>();
  }
  public void openDevice(MidiDeviceInfo deviceInfo, OnDeviceOpenedListener listener, android.os.Handler handler) {
    java.util.Objects.requireNonNull(deviceInfo);
    java.util.Objects.requireNonNull(listener);
    throw new IllegalArgumentException("device does not exist: " + deviceInfo);
  }
  /** AOSP reports a Bluetooth device that cannot be opened with a null device. */
  public void openBluetoothDevice(android.bluetooth.BluetoothDevice device, OnDeviceOpenedListener listener,
                                  android.os.Handler handler) {
    java.util.Objects.requireNonNull(listener);
    Runnable failed = () -> listener.onDeviceOpened(null);
    if (handler != null) handler.post(failed);
    else failed.run();
  }
  private static void checkTransport(int transport) {
    if (transport != TRANSPORT_MIDI_BYTE_STREAM && transport != TRANSPORT_UNIVERSAL_MIDI_PACKETS)
      throw new IllegalArgumentException("Invalid transport type: " + transport);
  }
}
