package android.media.midi;

/** An open MIDI device. MidiManager never opens one in Venture, so ports do not open. */
public final class MidiDevice implements java.io.Closeable {
  public final class MidiConnection implements java.io.Closeable {
    @Override public void close() {}
  }
  private final MidiDeviceInfo info;
  MidiDevice(MidiDeviceInfo info) { this.info = info; }
  public MidiDeviceInfo getInfo() { return info; }
  public MidiInputPort openInputPort(int portNumber) { return null; }
  public MidiOutputPort openOutputPort(int portNumber) { return null; }
  public MidiConnection connectPorts(MidiInputPort inputPort, int outputPortNumber) {
    if (outputPortNumber < 0 || outputPortNumber >= info.getOutputPortCount())
      throw new IllegalArgumentException("outputPortNumber out of range");
    return null;
  }
  @Override public void close() {}
  @Override public String toString() { return "MidiDevice: " + info; }
}
