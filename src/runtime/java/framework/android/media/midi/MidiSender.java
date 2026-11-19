package android.media.midi;

/** Sends MIDI bytes to connected receivers. */
public abstract class MidiSender {
  public void connect(MidiReceiver receiver) {
    if (receiver == null) throw new NullPointerException("receiver null in MidiSender.connect");
    onConnect(receiver);
  }
  public void disconnect(MidiReceiver receiver) {
    if (receiver == null) throw new NullPointerException("receiver null in MidiSender.disconnect");
    onDisconnect(receiver);
  }
  public abstract void onConnect(MidiReceiver receiver);
  public abstract void onDisconnect(MidiReceiver receiver);
}
