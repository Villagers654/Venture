package android.telephony;

/** AOSP android.telephony.TelephonyDisplayInfo (API 30). */
public final class TelephonyDisplayInfo {
  public static final int OVERRIDE_NETWORK_TYPE_NONE = 0, OVERRIDE_NETWORK_TYPE_LTE_CA = 1,
      OVERRIDE_NETWORK_TYPE_LTE_ADVANCED_PRO = 2, OVERRIDE_NETWORK_TYPE_NR_NSA = 3, OVERRIDE_NETWORK_TYPE_NR_ADVANCED = 5;
  private final int networkType, overrideNetworkType;
  public TelephonyDisplayInfo(int networkType, int overrideNetworkType) {
    this.networkType = networkType;
    this.overrideNetworkType = overrideNetworkType;
  }
  public int getNetworkType() { return networkType; }
  public int getOverrideNetworkType() { return overrideNetworkType; }
}
