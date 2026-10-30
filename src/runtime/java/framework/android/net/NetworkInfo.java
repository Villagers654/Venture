package android.net;
public final class NetworkInfo {
  private final int type;
  private final boolean connected;
  public NetworkInfo() { this(-1, false); }
  NetworkInfo(int type, boolean connected) { this.type = type; this.connected = connected; }
  public boolean isConnected() {
    return connected;
  }
  public boolean isConnectedOrConnecting() {
    return connected;
  }
  public boolean isAvailable() {
    // This snapshot represents a usable host route. Inactive transports have
    // no Android radio/data service that could make them available on demand.
    return connected;
  }
  public int getType() {
    return type;
  }
  // TelephonyManager.NETWORK_TYPE_UNKNOWN: the host's links are not cellular.
  public int getSubtype() {
    return 0;
  }
  public String getSubtypeName() {
    return "";
  }
  public String getTypeName() {
    return switch (type) {
      case ConnectivityManager.TYPE_WIFI -> "WIFI";
      case ConnectivityManager.TYPE_MOBILE -> "MOBILE";
      case ConnectivityManager.TYPE_ETHERNET -> "ETHERNET";
      case ConnectivityManager.TYPE_VPN -> "VPN";
      default -> "UNKNOWN";
    };
  }
}
