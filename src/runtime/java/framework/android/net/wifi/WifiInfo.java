package android.net.wifi;

// The connection a Quest app sees without location permission (Android 10+):
// SSID, BSSID, MAC address and network id are redacted, the radio values are
// real. Venture reads no host radio, so link speed and frequency are unknown;
// an associated host Wi-Fi reports a strong signal, as a Quest beside its
// router would, rather than INVALID_RSSI, which games show as no signal.
public class WifiInfo {
  public static final String UNKNOWN_SSID = "<unknown ssid>";
  public static final String DEFAULT_MAC_ADDRESS = "02:00:00:00:00:00";
  public static final int INVALID_RSSI = -127, LINK_SPEED_UNKNOWN = -1, FREQUENCY_UNKNOWN = -1;
  public static final String LINK_SPEED_UNITS = "Mbps", FREQUENCY_UNITS = "MHz";
  static final int ASSOCIATED_RSSI = -50;
  private final boolean associated;
  WifiInfo(boolean associated) { this.associated = associated; }
  public String getSSID() { return UNKNOWN_SSID; }
  public String getBSSID() { return associated ? DEFAULT_MAC_ADDRESS : null; }
  public String getMacAddress() { return DEFAULT_MAC_ADDRESS; }
  public int getNetworkId() { return -1; }
  public boolean getHiddenSSID() { return false; }
  public int getRssi() { return associated ? ASSOCIATED_RSSI : INVALID_RSSI; }
  public int getLinkSpeed() { return LINK_SPEED_UNKNOWN; }
  public int getTxLinkSpeedMbps() { return LINK_SPEED_UNKNOWN; }
  public int getRxLinkSpeedMbps() { return LINK_SPEED_UNKNOWN; }
  public int getMaxSupportedTxLinkSpeedMbps() { return LINK_SPEED_UNKNOWN; }
  public int getMaxSupportedRxLinkSpeedMbps() { return LINK_SPEED_UNKNOWN; }
  public int getFrequency() { return FREQUENCY_UNKNOWN; }
  public int getIpAddress() { return 0; }
  public int getWifiStandard() { return 0; }  // ScanResult.WIFI_STANDARD_UNKNOWN
  public SupplicantState getSupplicantState() {
    return associated ? SupplicantState.COMPLETED : SupplicantState.DISCONNECTED;
  }
  @Override public String toString() {
    return "SSID: " + getSSID() + ", BSSID: " + getBSSID() + ", MAC: " + getMacAddress()
        + ", Supplicant state: " + getSupplicantState() + ", RSSI: " + getRssi();
  }
}
