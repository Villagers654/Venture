package android.media;
/** Applications override the device events they consume. */
public abstract class AudioDeviceCallback {
  public void onAudioDevicesAdded(AudioDeviceInfo[] devices) {}
  public void onAudioDevicesRemoved(AudioDeviceInfo[] devices) {}
}
