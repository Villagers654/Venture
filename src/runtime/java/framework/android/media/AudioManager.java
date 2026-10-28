package android.media;
public class AudioManager {
  public static final String PROPERTY_OUTPUT_SAMPLE_RATE =
                                 "android.media.property.OUTPUT_SAMPLE_RATE",
                             PROPERTY_OUTPUT_FRAMES_PER_BUFFER =
                                 "android.media.property.OUTPUT_FRAMES_PER_BUFFER";
  public static final int GET_DEVICES_INPUTS = 1, GET_DEVICES_OUTPUTS = 2, GET_DEVICES_ALL = 3;
  public static final int STREAM_MUSIC = 3, AUDIOFOCUS_GAIN = 1, AUDIOFOCUS_REQUEST_GRANTED = 1,
                          AUDIOFOCUS_REQUEST_FAILED = 0;
  private int volume = 15;
  private OnAudioFocusChangeListener focus;
  private record Route(int id, boolean input, String name) {}
  private static final java.util.concurrent.ScheduledExecutorService inventoryWorker =
      java.util.concurrent.Executors.newSingleThreadScheduledExecutor(r -> {
        Thread t = new Thread(r, "Android-AudioInventory");
        t.setDaemon(true);
        return t;
      });
  private static final class Registration {
    final AudioDeviceCallback callback;
    final android.os.Handler handler;
    volatile boolean active = true;
    java.util.Map<Route, AudioDeviceInfo> previous = java.util.Map.of();
    Registration(AudioDeviceCallback callback, android.os.Handler handler) {
      this.callback = callback;
      this.handler = handler;
    }
  }
  private final java.util.Map<AudioDeviceCallback, Registration> deviceCallbacks =
      new java.util.IdentityHashMap<>();
  private java.util.concurrent.ScheduledFuture<?> inventoryPoll;
  public synchronized void registerAudioDeviceCallback(
      AudioDeviceCallback callback, android.os.Handler handler) {
    java.util.Objects.requireNonNull(callback);
    if (deviceCallbacks.containsKey(callback))
      return;
    deviceCallbacks.put(callback,
        new Registration(callback,
            handler == null ? new android.os.Handler(android.os.Looper.getMainLooper()) : handler));
    if (inventoryPoll == null)
      inventoryPoll = inventoryWorker.scheduleWithFixedDelay(
          this::pollDevices, 0, 1, java.util.concurrent.TimeUnit.SECONDS);
  }
  public synchronized void unregisterAudioDeviceCallback(AudioDeviceCallback callback) {
    Registration removed = deviceCallbacks.remove(callback);
    if (removed != null)
      removed.active = false;
    if (deviceCallbacks.isEmpty() && inventoryPoll != null) {
      inventoryPoll.cancel(false);
      inventoryPoll = null;
    }
  }
  private void pollDevices() {
    try {
      var current = new java.util.LinkedHashMap<Route, AudioDeviceInfo>();
      for (var device : getDevices(GET_DEVICES_ALL))
        current.put(
            new Route(device.getId(), device.isSource(), device.getProductName().toString()),
            device);
      synchronized (this) {
        for (Registration registration : deviceCallbacks.values()) {
          var removed = registration.previous.entrySet()
                            .stream()
                            .filter(e -> !current.containsKey(e.getKey()))
                            .map(java.util.Map.Entry::getValue)
                            .toArray(AudioDeviceInfo[] ::new);
          var added = current.entrySet()
                          .stream()
                          .filter(e -> !registration.previous.containsKey(e.getKey()))
                          .map(java.util.Map.Entry::getValue)
                          .toArray(AudioDeviceInfo[] ::new);
          registration.previous = java.util.Map.copyOf(current);
          if (removed.length != 0 || added.length != 0)
            registration.handler.post(() -> {
              if (!registration.active)
                return;
              if (removed.length != 0)
                registration.callback.onAudioDevicesRemoved(removed);
              if (registration.active && added.length != 0)
                registration.callback.onAudioDevicesAdded(added);
            });
        }
      }
    } catch (RuntimeException failure) {
      android.util.Log.e(
          "AudioManager", "Device inventory failed: " + failure.getClass().getSimpleName());
    }
  }
  public AudioDeviceInfo[] getDevices(int flags) {
    if ((flags & ~GET_DEVICES_ALL) != 0)
      throw new IllegalArgumentException("Audio device flags");
    return java.util.Arrays.stream(com.venture.runtime.RuntimeEnvironment.host().audioDevices())
        .filter(d
            -> d.isSource() ? (flags & GET_DEVICES_INPUTS) != 0
                            : (flags & GET_DEVICES_OUTPUTS) != 0)
        .toArray(AudioDeviceInfo[] ::new);
  }
  public interface OnAudioFocusChangeListener {
    void onAudioFocusChange(int change);
  }
  public synchronized int getStreamVolume(int stream) {
    return volume;
  }
  public int getStreamMaxVolume(int stream) {
    return 15;
  }
  public int getStreamMinVolume(int stream) {
    return 0;
  }
  public synchronized void setStreamVolume(int stream, int v, int flags) {
    volume = Math.max(0, Math.min(v, 15));
    android.util.Log.w("AudioManager", "Volume policy updated; host audio gain bridge unavailable");
  }
  public boolean isMusicActive() {
    return com.venture.runtime.RuntimeEnvironment.host().isMusicActive();
  }
  public String getProperty(String key) {
    final int index;
    if (PROPERTY_OUTPUT_SAMPLE_RATE.equals(key)) index = 0;
    else if (PROPERTY_OUTPUT_FRAMES_PER_BUFFER.equals(key)) index = 1;
    else return null;
    int value = com.venture.runtime.RuntimeEnvironment.host().audioOutputProperties()[index];
    return value > 0 ? Integer.toString(value) : null;
  }
  public boolean isBluetoothScoOn() {
    return false;
  }
  public boolean isBluetoothA2dpOn() {
    return false;
  }
  public boolean isWiredHeadsetOn() {
    return false;
  }
  public boolean isSpeakerphoneOn() {
    return false;
  }
  public int getMode() {
    return 0;
  }
  public synchronized int requestAudioFocus(
      OnAudioFocusChangeListener l, int stream, int duration) {
    if (focus != null && focus != l)
      focus.onAudioFocusChange(-1);
    focus = l;
    return 1;
  }
  public synchronized int abandonAudioFocus(OnAudioFocusChangeListener l) {
    if (focus == l)
      focus = null;
    return 1;
  }
  public void startBluetoothSco() {
    android.util.Log.w("AudioManager", "Bluetooth SCO unavailable");
  }
  public void stopBluetoothSco() {}
}
