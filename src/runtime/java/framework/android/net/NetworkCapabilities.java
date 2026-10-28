package android.net;

public final class NetworkCapabilities {
  public static final int NET_CAPABILITY_INTERNET = 12, NET_CAPABILITY_NOT_RESTRICTED = 13,
      NET_CAPABILITY_TRUSTED = 14, NET_CAPABILITY_NOT_VPN = 15, NET_CAPABILITY_VALIDATED = 16,
      NET_CAPABILITY_NOT_METERED = 11, NET_CAPABILITY_NOT_ROAMING = 18;
  // Android frameworks/base/core/java/android/net/NetworkCapabilities.java.
  public static final int TRANSPORT_CELLULAR = 0, TRANSPORT_WIFI = 1,
      TRANSPORT_ETHERNET = 3, TRANSPORT_VPN = 4;
  private final int type;
  private final boolean validated;
  NetworkCapabilities(int type, boolean validated) { this.type = type; this.validated = validated; }
  public boolean hasCapability(int capability) {
    // AOSP 12L default request requires INTERNET; VALIDATED is independent
    // evidence supplied by the host's connectivity service (NLM on Windows).
    return switch (capability) {
      case NET_CAPABILITY_NOT_RESTRICTED, NET_CAPABILITY_TRUSTED -> true;
      case NET_CAPABILITY_NOT_VPN -> type != ConnectivityManager.TYPE_VPN;
      case NET_CAPABILITY_INTERNET -> true;
      case NET_CAPABILITY_VALIDATED -> validated;
      // Preserve Quest's Wi-Fi policy for VPN/unknown host adapters; only a
      // positively identified cellular route is treated as metered/roaming.
      case NET_CAPABILITY_NOT_METERED, NET_CAPABILITY_NOT_ROAMING ->
          type != ConnectivityManager.TYPE_MOBILE;
      default -> false;
    };
  }
  @Override public boolean equals(Object other) {
    return other instanceof NetworkCapabilities caps && type == caps.type && validated == caps.validated;
  }
  @Override public int hashCode() { return 31 * type + (validated ? 1 : 0); }
  public boolean hasTransport(int transport) {
    return switch (transport) {
      case TRANSPORT_CELLULAR -> type == ConnectivityManager.TYPE_MOBILE;
      case TRANSPORT_WIFI -> type == ConnectivityManager.TYPE_WIFI;
      case TRANSPORT_ETHERNET -> type == ConnectivityManager.TYPE_ETHERNET;
      case TRANSPORT_VPN -> type == ConnectivityManager.TYPE_VPN;
      default -> false;
    };
  }
}
