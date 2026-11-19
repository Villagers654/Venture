package android.hardware.usb;

/** A USB device attached to the host. Venture exposes no Android USB host, so
 * UsbManager never reports one; the class exists for code that names it. */
public class UsbDevice implements android.os.Parcelable {
  private final String name, manufacturerName, productName, version, serialNumber;
  private final int vendorId, productId, deviceClass, subclass, protocol;
  private UsbConfiguration[] configurations = new UsbConfiguration[0];
  private UsbInterface[] interfaces;
  public UsbDevice(String name, int vendorId, int productId, int deviceClass, int subclass, int protocol,
                   String manufacturerName, String productName, String version, String serialNumber) {
    this.name = java.util.Objects.requireNonNull(name);
    this.vendorId = vendorId;
    this.productId = productId;
    this.deviceClass = deviceClass;
    this.subclass = subclass;
    this.protocol = protocol;
    this.manufacturerName = manufacturerName;
    this.productName = productName;
    this.version = version;
    this.serialNumber = serialNumber;
  }
  public String getDeviceName() { return name; }
  public String getManufacturerName() { return manufacturerName; }
  public String getProductName() { return productName; }
  public String getVersion() { return version; }
  public String getSerialNumber() { return serialNumber; }
  public int getDeviceId() { return getDeviceId(name); }
  public int getVendorId() { return vendorId; }
  public int getProductId() { return productId; }
  public int getDeviceClass() { return deviceClass; }
  public int getDeviceSubclass() { return subclass; }
  public int getDeviceProtocol() { return protocol; }
  public int getConfigurationCount() { return configurations.length; }
  public UsbConfiguration getConfiguration(int index) { return configurations[index]; }
  public void setConfigurations(android.os.Parcelable[] configurations) {
    this.configurations = java.util.Arrays.copyOf(configurations, configurations.length, UsbConfiguration[].class);
    interfaces = null;
  }
  // Every interface of every configuration, as AOSP flattens them.
  private UsbInterface[] interfaceList() {
    if (interfaces == null) {
      var all = new java.util.ArrayList<UsbInterface>();
      for (UsbConfiguration configuration : configurations)
        for (int i = 0; i < configuration.getInterfaceCount(); i++) all.add(configuration.getInterface(i));
      interfaces = all.toArray(new UsbInterface[0]);
    }
    return interfaces;
  }
  public int getInterfaceCount() { return interfaceList().length; }
  public UsbInterface getInterface(int index) { return interfaceList()[index]; }
  // AOSP derives the ID from the /dev/bus/usb/BBB/DDD path.
  public static int getDeviceId(String name) {
    String[] parts = name.split("/");
    try {
      return parts.length < 2 ? -1
          : Integer.parseInt(parts[parts.length - 2]) * 1000 + Integer.parseInt(parts[parts.length - 1]);
    } catch (NumberFormatException e) {
      return -1;
    }
  }
  public static String getDeviceName(int id) {
    return String.format(java.util.Locale.ROOT, "/dev/bus/usb/%03d/%03d", id / 1000, id % 1000);
  }
  @Override public boolean equals(Object o) {
    if (o instanceof UsbDevice other) return name.equals(other.name);
    return o instanceof String s && name.equals(s);
  }
  @Override public int hashCode() { return name.hashCode(); }
  @Override public String toString() {
    return "UsbDevice[mName=" + name + ",mVendorId=" + vendorId + ",mProductId=" + productId + ",mClass=" + deviceClass
        + ",mSubclass=" + subclass + ",mProtocol=" + protocol + ",mManufacturerName=" + manufacturerName
        + ",mProductName=" + productName + ",mVersion=" + version + "]";
  }
  @Override public int describeContents() { return 0; }
  @Override public void writeToParcel(android.os.Parcel dest, int flags) {
    dest.writeString(name);
    dest.writeInt(vendorId);
    dest.writeInt(productId);
    dest.writeInt(deviceClass);
    dest.writeInt(subclass);
    dest.writeInt(protocol);
    dest.writeString(manufacturerName);
    dest.writeString(productName);
    dest.writeString(version);
    dest.writeString(serialNumber);
    dest.writeInt(configurations.length);
    for (UsbConfiguration configuration : configurations) configuration.writeToParcel(dest, flags);
  }
  public static final Creator<UsbDevice> CREATOR = new Creator<>() {
    @Override public UsbDevice createFromParcel(android.os.Parcel in) {
      var result = new UsbDevice(in.readString(), in.readInt(), in.readInt(), in.readInt(), in.readInt(), in.readInt(),
          in.readString(), in.readString(), in.readString(), in.readString());
      var configurations = new UsbConfiguration[in.readInt()];
      for (int i = 0; i < configurations.length; i++) configurations[i] = UsbConfiguration.CREATOR.createFromParcel(in);
      result.configurations = configurations;
      return result;
    }
    @Override public UsbDevice[] newArray(int size) { return new UsbDevice[size]; }
  };
}
