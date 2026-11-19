package android.hardware.usb;

/** One configuration of a UsbDevice and its interfaces. */
public class UsbConfiguration implements android.os.Parcelable {
  private final int id, attributes, maxPower;
  private final String name;
  private UsbInterface[] interfaces = new UsbInterface[0];
  public UsbConfiguration(int id, String name, int attributes, int maxPower) {
    this.id = id;
    this.name = name;
    this.attributes = attributes;
    this.maxPower = maxPower;
  }
  public int getId() { return id; }
  public String getName() { return name; }
  public boolean isSelfPowered() { return (attributes & 0x40) != 0; }
  public boolean isRemoteWakeup() { return (attributes & 0x20) != 0; }
  public int getAttributes() { return attributes; }
  /** In milliamps; the descriptor counts 2 mA units. */
  public int getMaxPower() { return maxPower * 2; }
  public int getInterfaceCount() { return interfaces.length; }
  public UsbInterface getInterface(int index) { return interfaces[index]; }
  public void setInterfaces(android.os.Parcelable[] interfaces) {
    this.interfaces = java.util.Arrays.copyOf(interfaces, interfaces.length, UsbInterface[].class);
  }
  @Override public int describeContents() { return 0; }
  @Override public void writeToParcel(android.os.Parcel dest, int flags) {
    dest.writeInt(id);
    dest.writeString(name);
    dest.writeInt(attributes);
    dest.writeInt(maxPower);
    dest.writeInt(interfaces.length);
    for (UsbInterface usbInterface : interfaces) usbInterface.writeToParcel(dest, flags);
  }
  public static final Creator<UsbConfiguration> CREATOR = new Creator<>() {
    @Override public UsbConfiguration createFromParcel(android.os.Parcel in) {
      var result = new UsbConfiguration(in.readInt(), in.readString(), in.readInt(), in.readInt());
      var interfaces = new UsbInterface[in.readInt()];
      for (int i = 0; i < interfaces.length; i++) interfaces[i] = UsbInterface.CREATOR.createFromParcel(in);
      result.interfaces = interfaces;
      return result;
    }
    @Override public UsbConfiguration[] newArray(int size) { return new UsbConfiguration[size]; }
  };
}
