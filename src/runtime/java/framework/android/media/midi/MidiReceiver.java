package android.media.midi;

/** Receives MIDI bytes. */
public abstract class MidiReceiver {
  private final int maxMessageSize;
  public MidiReceiver() { this(Integer.MAX_VALUE); }
  public MidiReceiver(int maxMessageSize) { this.maxMessageSize = maxMessageSize; }
  public abstract void onSend(byte[] msg, int offset, int count, long timestamp) throws java.io.IOException;
  public void onFlush() throws java.io.IOException {}
  public final int getMaxMessageSize() { return maxMessageSize; }
  public void send(byte[] msg, int offset, int count) throws java.io.IOException {
    send(msg, offset, count, System.nanoTime());
  }
  public void send(byte[] msg, int offset, int count, long timestamp) throws java.io.IOException {
    while (count > 0) {
      int length = Math.min(count, maxMessageSize);
      onSend(msg, offset, length, timestamp);
      offset += length;
      count -= length;
    }
  }
  public void flush() throws java.io.IOException { onFlush(); }
}
