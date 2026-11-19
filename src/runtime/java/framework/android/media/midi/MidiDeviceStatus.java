package android.media.midi;

/** The open state of a MIDI device's ports. */
public final class MidiDeviceStatus implements android.os.Parcelable {
  private final MidiDeviceInfo info;
  private final boolean[] inputPortOpen;
  private final int[] outputPortOpenCount;
  public MidiDeviceStatus(MidiDeviceInfo info) {
    this.info = info;
    inputPortOpen = new boolean[info.getInputPortCount()];
    outputPortOpenCount = new int[info.getOutputPortCount()];
  }
  public MidiDeviceInfo getDeviceInfo() { return info; }
  public boolean isInputPortOpen(int portNumber) { return inputPortOpen[portNumber]; }
  public int getOutputPortOpenCount(int portNumber) { return outputPortOpenCount[portNumber]; }
  @Override public int describeContents() { return 0; }
  @Override public void writeToParcel(android.os.Parcel dest, int flags) { info.writeToParcel(dest, flags); }
  public static final Creator<MidiDeviceStatus> CREATOR = new Creator<>() {
    @Override public MidiDeviceStatus createFromParcel(android.os.Parcel in) {
      return new MidiDeviceStatus(MidiDeviceInfo.CREATOR.createFromParcel(in));
    }
    @Override public MidiDeviceStatus[] newArray(int size) { return new MidiDeviceStatus[size]; }
  };
}
