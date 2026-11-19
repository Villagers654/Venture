package android.hardware.usb;

/** One interface of a UsbConfiguration and its endpoints. */
public class UsbInterface implements android.os.Parcelable {
  private final int id, alternateSetting, interfaceClass, subclass, protocol;
  private final String name;
  private UsbEndpoint[] endpoints = new UsbEndpoint[0];
  public UsbInterface(int id, int alternateSetting, String name, int interfaceClass, int subclass, int protocol) {
    this.id = id;
    this.alternateSetting = alternateSetting;
    this.name = name;
    this.interfaceClass = interfaceClass;
    this.subclass = subclass;
    this.protocol = protocol;
  }
  public int getId() { return id; }
  public int getAlternateSetting() { return alternateSetting; }
  public String getName() { return name; }
  public int getInterfaceClass() { return interfaceClass; }
  public int getInterfaceSubclass() { return subclass; }
  public int getInterfaceProtocol() { return protocol; }
  public int getEndpointCount() { return endpoints.length; }
  public UsbEndpoint getEndpoint(int index) { return endpoints[index]; }
  public void setEndpoints(android.os.Parcelable[] endpoints) {
    this.endpoints = java.util.Arrays.copyOf(endpoints, endpoints.length, UsbEndpoint[].class);
  }
  @Override public String toString() {
    return "UsbInterface[mId=" + id + ",mAlternateSetting=" + alternateSetting + ",mName=" + name + ",mClass="
        + interfaceClass + ",mSubclass=" + subclass + ",mProtocol=" + protocol + "]";
  }
  @Override public int describeContents() { return 0; }
  @Override public void writeToParcel(android.os.Parcel dest, int flags) {
    dest.writeInt(id);
    dest.writeInt(alternateSetting);
    dest.writeString(name);
    dest.writeInt(interfaceClass);
    dest.writeInt(subclass);
    dest.writeInt(protocol);
    dest.writeInt(endpoints.length);
    for (UsbEndpoint endpoint : endpoints) endpoint.writeToParcel(dest, flags);
  }
  public static final Creator<UsbInterface> CREATOR = new Creator<>() {
    @Override public UsbInterface createFromParcel(android.os.Parcel in) {
      var result = new UsbInterface(in.readInt(), in.readInt(), in.readString(), in.readInt(), in.readInt(), in.readInt());
      var endpoints = new UsbEndpoint[in.readInt()];
      for (int i = 0; i < endpoints.length; i++) endpoints[i] = UsbEndpoint.CREATOR.createFromParcel(in);
      result.endpoints = endpoints;
      return result;
    }
    @Override public UsbInterface[] newArray(int size) { return new UsbInterface[size]; }
  };
}
