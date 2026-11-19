package android.media.midi;

/** Describes a MIDI device. MidiManager reports none in Venture. */
public final class MidiDeviceInfo implements android.os.Parcelable {
  public static final int TYPE_USB = 1, TYPE_VIRTUAL = 2, TYPE_BLUETOOTH = 3;
  public static final int PROTOCOL_UNKNOWN = -1;
  public static final String PROPERTY_NAME = "name", PROPERTY_MANUFACTURER = "manufacturer",
      PROPERTY_PRODUCT = "product", PROPERTY_VERSION = "version", PROPERTY_SERIAL_NUMBER = "serial_number",
      PROPERTY_USB_DEVICE = "usb_device", PROPERTY_BLUETOOTH_DEVICE = "bluetooth_device",
      PROPERTY_ALSA_CARD = "alsa_card", PROPERTY_ALSA_DEVICE = "alsa_device",
      PROPERTY_SERVICE_INFO = "service_info";
  public static final class PortInfo {
    public static final int TYPE_INPUT = 1, TYPE_OUTPUT = 2;
    private final int type, portNumber;
    private final String name;
    PortInfo(int type, int portNumber, String name) {
      this.type = type;
      this.portNumber = portNumber;
      this.name = name == null ? "" : name;
    }
    public int getType() { return type; }
    public int getPortNumber() { return portNumber; }
    public String getName() { return name; }
  }
  private final int type, id, inputPortCount, outputPortCount, defaultProtocol;
  private final String[] inputPortNames, outputPortNames;
  private final android.os.Bundle properties;
  private final boolean isPrivate;
  public MidiDeviceInfo(int type, int id, int numInputPorts, int numOutputPorts, String[] inputPortNames,
                        String[] outputPortNames, android.os.Bundle properties, boolean isPrivate, int defaultProtocol) {
    this.type = type;
    this.id = id;
    this.inputPortCount = numInputPorts;
    this.outputPortCount = numOutputPorts;
    this.inputPortNames = inputPortNames == null ? new String[numInputPorts] : inputPortNames.clone();
    this.outputPortNames = outputPortNames == null ? new String[numOutputPorts] : outputPortNames.clone();
    this.properties = properties == null ? new android.os.Bundle() : properties;
    this.isPrivate = isPrivate;
    this.defaultProtocol = defaultProtocol;
  }
  public int getType() { return type; }
  public int getId() { return id; }
  public int getInputPortCount() { return inputPortCount; }
  public int getOutputPortCount() { return outputPortCount; }
  public PortInfo[] getPorts() {
    var ports = new PortInfo[inputPortCount + outputPortCount];
    int index = 0;
    for (int i = 0; i < inputPortCount; i++) ports[index++] = new PortInfo(PortInfo.TYPE_INPUT, i, inputPortNames[i]);
    for (int i = 0; i < outputPortCount; i++) ports[index++] = new PortInfo(PortInfo.TYPE_OUTPUT, i, outputPortNames[i]);
    return ports;
  }
  public android.os.Bundle getProperties() { return properties; }
  public boolean isPrivate() { return isPrivate; }
  public int getDefaultProtocol() { return defaultProtocol; }
  @Override public boolean equals(Object o) { return o instanceof MidiDeviceInfo other && other.id == id; }
  @Override public int hashCode() { return id; }
  @Override public String toString() {
    return "MidiDeviceInfo[mType=" + type + ",mInputPortCount=" + inputPortCount + ",mOutputPortCount="
        + outputPortCount + ",mProperties=" + properties + ",mIsPrivate=" + isPrivate + "]";
  }
  @Override public int describeContents() { return 0; }
  @Override public void writeToParcel(android.os.Parcel dest, int flags) {
    dest.writeInt(type);
    dest.writeInt(id);
    dest.writeInt(inputPortCount);
    dest.writeInt(outputPortCount);
    dest.writeStringArray(inputPortNames);
    dest.writeStringArray(outputPortNames);
    dest.writeBundle(properties);
    dest.writeInt(isPrivate ? 1 : 0);
    dest.writeInt(defaultProtocol);
  }
  public static final Creator<MidiDeviceInfo> CREATOR = new Creator<>() {
    @Override public MidiDeviceInfo createFromParcel(android.os.Parcel in) {
      return new MidiDeviceInfo(in.readInt(), in.readInt(), in.readInt(), in.readInt(), in.createStringArray(),
          in.createStringArray(), in.readBundle(), in.readInt() != 0, in.readInt());
    }
    @Override public MidiDeviceInfo[] newArray(int size) { return new MidiDeviceInfo[size]; }
  };
}
