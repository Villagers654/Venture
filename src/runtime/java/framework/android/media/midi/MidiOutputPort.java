package android.media.midi;

/** A device output port. MidiDevice never opens one in Venture. */
public final class MidiOutputPort extends MidiSender implements java.io.Closeable {
  private final int portNumber;
  private final java.util.Set<MidiReceiver> receivers = java.util.concurrent.ConcurrentHashMap.newKeySet();
  MidiOutputPort(int portNumber) { this.portNumber = portNumber; }
  public int getPortNumber() { return portNumber; }
  @Override public void onConnect(MidiReceiver receiver) { receivers.add(receiver); }
  @Override public void onDisconnect(MidiReceiver receiver) { receivers.remove(receiver); }
  @Override public void close() { receivers.clear(); }
}
