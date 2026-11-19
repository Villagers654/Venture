package android.hardware.usb;

/** A USB accessory in accessory mode. Venture exposes none. */
public class UsbAccessory implements android.os.Parcelable {
  private final String manufacturer, model, description, version, uri, serial;
  public UsbAccessory(String manufacturer, String model, String description, String version, String uri, String serial) {
    this.manufacturer = java.util.Objects.requireNonNull(manufacturer);
    this.model = java.util.Objects.requireNonNull(model);
    this.description = description;
    this.version = version;
    this.uri = uri;
    this.serial = serial;
  }
  public String getManufacturer() { return manufacturer; }
  public String getModel() { return model; }
  public String getDescription() { return description; }
  public String getVersion() { return version; }
  public String getUri() { return uri; }
  public String getSerial() { return serial; }
  @Override public boolean equals(Object o) {
    return o instanceof UsbAccessory a && manufacturer.equals(a.manufacturer) && model.equals(a.model)
        && java.util.Objects.equals(description, a.description) && java.util.Objects.equals(version, a.version)
        && java.util.Objects.equals(uri, a.uri) && java.util.Objects.equals(serial, a.serial);
  }
  @Override public int hashCode() {
    return java.util.Objects.hash(manufacturer, model, description, version, uri, serial);
  }
  @Override public String toString() {
    return "UsbAccessory[mManufacturer=" + manufacturer + ", mModel=" + model + ", mDescription=" + description
        + ", mVersion=" + version + ", mUri=" + uri + "]";
  }
  @Override public int describeContents() { return 0; }
  @Override public void writeToParcel(android.os.Parcel dest, int flags) {
    dest.writeString(manufacturer);
    dest.writeString(model);
    dest.writeString(description);
    dest.writeString(version);
    dest.writeString(uri);
    dest.writeString(serial);
  }
  public static final Creator<UsbAccessory> CREATOR = new Creator<>() {
    @Override public UsbAccessory createFromParcel(android.os.Parcel in) {
      return new UsbAccessory(in.readString(), in.readString(), in.readString(), in.readString(), in.readString(),
          in.readString());
    }
    @Override public UsbAccessory[] newArray(int size) { return new UsbAccessory[size]; }
  };
}
