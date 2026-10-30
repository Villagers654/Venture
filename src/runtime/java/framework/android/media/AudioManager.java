package android.media;
public class AudioManager {
  public static final String PROPERTY_OUTPUT_SAMPLE_RATE =
                                 "android.media.property.OUTPUT_SAMPLE_RATE",
                             PROPERTY_OUTPUT_FRAMES_PER_BUFFER =
                                 "android.media.property.OUTPUT_FRAMES_PER_BUFFER";
  public static final int GET_DEVICES_INPUTS = 1, GET_DEVICES_OUTPUTS = 2, GET_DEVICES_ALL = 3;
  public static final int STREAM_VOICE_CALL = 0, STREAM_SYSTEM = 1, STREAM_RING = 2, STREAM_ALARM = 4,
                          STREAM_NOTIFICATION = 5, STREAM_DTMF = 8, STREAM_ACCESSIBILITY = 10;
  public static final int STREAM_MUSIC = 3, AUDIOFOCUS_GAIN = 1, AUDIOFOCUS_REQUEST_GRANTED = 1,
                          AUDIOFOCUS_REQUEST_FAILED = 0;
  public static final int AUDIOFOCUS_NONE = 0, AUDIOFOCUS_GAIN_TRANSIENT = 2, AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK = 3,
                          AUDIOFOCUS_GAIN_TRANSIENT_EXCLUSIVE = 4, AUDIOFOCUS_LOSS = -1, AUDIOFOCUS_LOSS_TRANSIENT = -2,
                          AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK = -3, AUDIOFOCUS_REQUEST_DELAYED = 2;
  public static final int ADJUST_RAISE = 1, ADJUST_LOWER = -1, ADJUST_SAME = 0, ADJUST_MUTE = -100,
                          ADJUST_UNMUTE = 100, ADJUST_TOGGLE_MUTE = 101;
  public static final int FLAG_SHOW_UI = 1, FLAG_PLAY_SOUND = 4, FLAG_VIBRATE = 16;
  public static final int ERROR = -1, ERROR_DEAD_OBJECT = -6, AUDIO_SESSION_ID_GENERATE = 0;
  public static final int PLAYBACK_OFFLOAD_NOT_SUPPORTED = 0, PLAYBACK_OFFLOAD_SUPPORTED = 1,
                          PLAYBACK_OFFLOAD_GAPLESS_SUPPORTED = 2;
  public static final String ACTION_AUDIO_BECOMING_NOISY = "android.media.AUDIO_BECOMING_NOISY",
                             ACTION_HDMI_AUDIO_PLUG = "android.media.action.HDMI_AUDIO_PLUG";
  private static final java.util.concurrent.atomic.AtomicInteger sessions = new java.util.concurrent.atomic.AtomicInteger(1);
  private boolean muted;
  public static final int MODE_INVALID = -2, MODE_CURRENT = -1, MODE_NORMAL = 0, MODE_RINGTONE = 1,
                          MODE_IN_CALL = 2, MODE_IN_COMMUNICATION = 3, MODE_CALL_SCREENING = 4;
  private int volume = 15, mode = MODE_NORMAL;
  private boolean microphoneMute, speakerphone;
  private AudioDeviceInfo communicationDevice;
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
    // Android always answers both: without an open output device, report the
    // mixer's own 48 kHz and its 5 ms period.
    return Integer.toString(value > 0 ? value : index == 0 ? 48000 : 240);
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
  public synchronized boolean isSpeakerphoneOn() {
    return speakerphone;
  }
  // Voice chat (Photon Voice, WebRTC) asks for communication routing; the
  // host mixer keeps its default devices, so the choices are only recorded.
  public synchronized int getMode() {
    return mode;
  }
  public synchronized void setMode(int value) {
    if (value >= MODE_NORMAL && value <= MODE_CALL_SCREENING)
      mode = value;
  }
  public synchronized boolean isMicrophoneMute() {
    return microphoneMute;
  }
  public synchronized void setMicrophoneMute(boolean on) {
    microphoneMute = on;
  }
  public synchronized void setSpeakerphoneOn(boolean on) {
    speakerphone = on;
  }
  public boolean isBluetoothScoAvailableOffCall() {
    return false;
  }
  public void setBluetoothScoOn(boolean on) {}
  public java.util.List<AudioDeviceInfo> getAvailableCommunicationDevices() {
    return java.util.List.of(getDevices(GET_DEVICES_OUTPUTS));
  }
  public synchronized boolean setCommunicationDevice(AudioDeviceInfo device) {
    java.util.Objects.requireNonNull(device);
    if (!device.isSink())
      throw new IllegalArgumentException("Invalid device: " + device);
    for (var available : getAvailableCommunicationDevices())
      if (available.getId() == device.getId()) {
        communicationDevice = available;
        return true;
      }
    return false;
  }
  public synchronized AudioDeviceInfo getCommunicationDevice() {
    if (communicationDevice != null)
      return communicationDevice;
    var devices = getAvailableCommunicationDevices();
    return devices.isEmpty() ? null : devices.get(0);
  }
  public synchronized void clearCommunicationDevice() {
    communicationDevice = null;
  }
  public synchronized int requestAudioFocus(
      OnAudioFocusChangeListener l, int stream, int duration) {
    if (focus != null && focus != l)
      focus.onAudioFocusChange(-1);
    focus = l;
    return 1;
  }
  public int requestAudioFocus(AudioFocusRequest request) {
    return requestAudioFocus(java.util.Objects.requireNonNull(request).listener(), STREAM_MUSIC, request.getFocusGain());
  }
  public int abandonAudioFocusRequest(AudioFocusRequest request) {
    return abandonAudioFocus(java.util.Objects.requireNonNull(request).listener());
  }
  // Unique positive ids; AOSP hands out session ids in steps of 8.
  public int generateAudioSessionId() { return sessions.getAndIncrement() << 3 | 1; }
  public synchronized boolean isStreamMute(int stream) { return muted; }
  public synchronized void setStreamMute(int stream, boolean state) { muted = state; }
  public synchronized void adjustStreamVolume(int stream, int direction, int flags) {
    switch (direction) {
      case ADJUST_RAISE, ADJUST_LOWER -> setStreamVolume(stream, volume + direction, flags);
      case ADJUST_MUTE -> muted = true;
      case ADJUST_UNMUTE -> muted = false;
      case ADJUST_TOGGLE_MUTE -> muted = !muted;
      case ADJUST_SAME -> {}
      default -> throw new IllegalArgumentException("Bad direction " + direction);
    }
  }
  public Spatializer getSpatializer() { return new Spatializer(); }
  public static boolean isOffloadedPlaybackSupported(AudioFormat format, AudioAttributes attributes) { return false; }
  public static int getPlaybackOffloadSupport(AudioFormat format, AudioAttributes attributes) {
    return PLAYBACK_OFFLOAD_NOT_SUPPORTED;
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
