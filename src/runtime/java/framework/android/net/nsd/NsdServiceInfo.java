package android.net.nsd;

import android.net.Network;
import android.os.Parcel;
import android.os.Parcelable;
import java.net.InetAddress;
import java.net.UnknownHostException;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.Map;
import java.util.TreeMap;

/** A DNS-SD service description (API 32 fields: name, type, host, port, TXT attributes, network). */
public final class NsdServiceInfo implements Parcelable {
  private String serviceName, serviceType;
  private InetAddress host;
  private int port;
  private Network network;
  // AOSP keeps TXT keys case-insensitively in insertion-independent order.
  private final Map<String, byte[]> attributes = new TreeMap<>(String.CASE_INSENSITIVE_ORDER);

  public NsdServiceInfo() {}
  public String getServiceName() { return serviceName; }
  public void setServiceName(String name) { serviceName = name; }
  public String getServiceType() { return serviceType; }
  public void setServiceType(String type) { serviceType = type; }
  public InetAddress getHost() { return host; }
  public void setHost(InetAddress host) { this.host = host; }
  public int getPort() { return port; }
  public void setPort(int port) { this.port = port; }
  public Network getNetwork() { return network; }
  public void setNetwork(Network network) { this.network = network; }

  // RFC 6763 TXT rules as AOSP checks them: printable US-ASCII keys without
  // '=', key plus value at most 255 bytes, the whole record at most 1300.
  public void setAttribute(String key, String value) {
    byte[] bytes = value == null ? null : value.getBytes(StandardCharsets.UTF_8);
    if (key == null || key.isEmpty()) throw new IllegalArgumentException("Key cannot be empty");
    for (int i = 0; i < key.length(); ++i) {
      char c = key.charAt(i);
      if (c < 0x20 || c > 0x7e) throw new IllegalArgumentException("Key strings must be printable US-ASCII");
      if (c == '=') throw new IllegalArgumentException("Key strings must not include '='");
    }
    if (key.length() + (bytes == null ? 0 : bytes.length) >= 255)
      throw new IllegalArgumentException("Key length + value length must be < 255 bytes");
    if (key.length() > 9) android.util.Log.w("NsdServiceInfo", "Key lengths > 9 are discouraged: " + key);
    int size = 0;
    for (Map.Entry<String, byte[]> e : attributes.entrySet())
      if (!e.getKey().equalsIgnoreCase(key)) size += e.getKey().length() + (e.getValue() == null ? 0 : e.getValue().length) + 2;
    size += key.length() + (bytes == null ? 0 : bytes.length) + 2;
    if (size > 1300) throw new IllegalArgumentException("Total length of attributes must be < 1300 bytes");
    else if (size > 400) android.util.Log.w("NsdServiceInfo", "Total length of all attributes exceeds 400 bytes; truncation may occur");
    attributes.put(key, bytes);
  }
  public void removeAttribute(String key) { attributes.remove(key); }
  public Map<String, byte[]> getAttributes() { return Collections.unmodifiableMap(attributes); }

  @Override public String toString() {
    return "name: " + serviceName + ", type: " + serviceType + ", host: " + host + ", port: " + port
        + (network == null ? "" : ", network: " + network);
  }
  @Override public int describeContents() { return 0; }
  @Override public void writeToParcel(Parcel dest, int flags) {
    dest.writeString(serviceName);
    dest.writeString(serviceType);
    dest.writeInt(host == null ? 0 : 1);
    if (host != null) dest.writeByteArray(host.getAddress());
    dest.writeInt(port);
    dest.writeInt(attributes.size());
    for (Map.Entry<String, byte[]> e : attributes.entrySet()) {
      dest.writeString(e.getKey());
      dest.writeInt(e.getValue() == null ? -1 : 1);
      if (e.getValue() != null) dest.writeByteArray(e.getValue());
    }
  }
  public static final Creator<NsdServiceInfo> CREATOR = new Creator<>() {
    @Override public NsdServiceInfo createFromParcel(Parcel in) {
      NsdServiceInfo info = new NsdServiceInfo();
      info.serviceName = in.readString();
      info.serviceType = in.readString();
      if (in.readInt() == 1) {
        try {
          info.host = InetAddress.getByAddress(in.createByteArray());
        } catch (UnknownHostException e) {
          info.host = null;
        }
      }
      info.port = in.readInt();
      for (int n = in.readInt(); n > 0; --n) {
        String key = in.readString();
        info.attributes.put(key, in.readInt() == -1 ? null : in.createByteArray());
      }
      return info;
    }
    @Override public NsdServiceInfo[] newArray(int size) { return new NsdServiceInfo[size]; }
  };
}
