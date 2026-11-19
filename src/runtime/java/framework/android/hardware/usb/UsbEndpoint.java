package android.hardware.usb;

/** One endpoint of a UsbInterface. */
public class UsbEndpoint implements android.os.Parcelable {
  private final int address, attributes, maxPacketSize, interval;
  public UsbEndpoint(int address, int attributes, int maxPacketSize, int interval) {
    this.address = address;
    this.attributes = attributes;
    this.maxPacketSize = maxPacketSize;
    this.interval = interval;
  }
  public int getAddress() { return address; }
  public int getEndpointNumber() { return address & UsbConstants.USB_ENDPOINT_NUMBER_MASK; }
  public int getDirection() { return address & UsbConstants.USB_ENDPOINT_DIR_MASK; }
  public int getAttributes() { return attributes; }
  public int getType() { return attributes & UsbConstants.USB_ENDPOINT_XFERTYPE_MASK; }
  public int getMaxPacketSize() { return maxPacketSize; }
  public int getInterval() { return interval; }
  @Override public String toString() {
    return "UsbEndpoint[mAddress=" + address + ",mAttributes=" + attributes + ",mMaxPacketSize=" + maxPacketSize
        + ",mInterval=" + interval + "]";
  }
  @Override public int describeContents() { return 0; }
  @Override public void writeToParcel(android.os.Parcel dest, int flags) {
    dest.writeInt(address);
    dest.writeInt(attributes);
    dest.writeInt(maxPacketSize);
    dest.writeInt(interval);
  }
  public static final Creator<UsbEndpoint> CREATOR = new Creator<>() {
    @Override public UsbEndpoint createFromParcel(android.os.Parcel in) {
      return new UsbEndpoint(in.readInt(), in.readInt(), in.readInt(), in.readInt());
    }
    @Override public UsbEndpoint[] newArray(int size) { return new UsbEndpoint[size]; }
  };
}
