package android.media.midi;

/** A device input port. MidiDevice never opens one in Venture. */
public final class MidiInputPort extends MidiReceiver implements java.io.Closeable {
  private final int portNumber;
  MidiInputPort(int portNumber) { this.portNumber = portNumber; }
  public int getPortNumber() { return portNumber; }
  @Override public void onSend(byte[] msg, int offset, int count, long timestamp) throws java.io.IOException {
    throw new java.io.IOException("MidiInputPort is closed");
  }
  @Override public void close() {}
}
