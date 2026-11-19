package android.media.midi;

/** A virtual MIDI device an app publishes. Venture's MidiManager does not list
 * virtual devices, so the service binds but its device is never reported. */
public abstract class MidiDeviceService extends android.app.Service {
  public static final String SERVICE_INTERFACE = "android.media.midi.MidiDeviceService";
  public abstract MidiReceiver[] onGetInputPortReceivers();
  public final MidiReceiver[] getOutputPortReceivers() { return new MidiReceiver[0]; }
  public final MidiDeviceInfo getDeviceInfo() { return null; }
  public void onDeviceStatusChanged(MidiDeviceStatus status) {}
  public void onClose() {}
  @Override public android.os.IBinder onBind(android.content.Intent intent) { return null; }
}
