package android.telephony;
// A Quest has no telephony hardware: AOSP's TelephonyManager on a device
// without the phone service answers with no SIM, no modem, no subscription
// and empty operator names (analytics SDKs read the carrier at startup).
public class TelephonyManager {
  public static final int PHONE_TYPE_NONE = 0, PHONE_TYPE_GSM = 1, PHONE_TYPE_CDMA = 2, PHONE_TYPE_SIP = 3;
  public static final int NETWORK_TYPE_UNKNOWN = 0;
  public static final int SIM_STATE_UNKNOWN = 0, SIM_STATE_ABSENT = 1, SIM_STATE_READY = 5;
  public static final int CALL_STATE_IDLE = 0;
  public static final int DATA_UNKNOWN = -1, DATA_DISCONNECTED = 0, DATA_ACTIVITY_NONE = 0;
  public static final int UNKNOWN_CARRIER_ID = -1;
  public int getPhoneType() {
    return PHONE_TYPE_NONE;
  }
  public int getCallState() {
    return CALL_STATE_IDLE;
  }
  public int getNetworkType() {
    return NETWORK_TYPE_UNKNOWN;
  }
  public int getDataNetworkType() { return NETWORK_TYPE_UNKNOWN; }
  public int getVoiceNetworkType() { return NETWORK_TYPE_UNKNOWN; }
  public int getDataState() { return DATA_DISCONNECTED; }
  public int getDataActivity() { return DATA_ACTIVITY_NONE; }
  public boolean isDataEnabled() { return false; }
  public boolean isNetworkRoaming() { return false; }
  public String getNetworkOperator() {
    return "";
  }
  public String getNetworkOperatorName() { return ""; }
  public String getNetworkCountryIso() {
    return "";
  }
  public String getSimOperator() { return ""; }
  public String getSimOperatorName() { return ""; }
  public String getSimCountryIso() {
    return "";
  }
  // SubscriptionManager answers UNKNOWN when there is no subscription service.
  public int getSimState() { return SIM_STATE_UNKNOWN; }
  public int getSimState(int slotIndex) { return SIM_STATE_UNKNOWN; }
  public int getSimCarrierId() { return UNKNOWN_CARRIER_ID; }
  public CharSequence getSimCarrierIdName() { return null; }
  public boolean hasIccCard() { return false; }
  // Identifiers come from the phone service, which is absent.
  public String getDeviceId() { return null; }
  public String getDeviceId(int slotIndex) { return null; }
  public String getImei() { return null; }
  public String getImei(int slotIndex) { return null; }
  public String getMeid() { return null; }
  public String getSubscriberId() { return null; }
  public String getSimSerialNumber() { return null; }
  public String getLine1Number() { return null; }
  public String getVoiceMailNumber() { return null; }
  public int getSubscriptionId() { return -1; }  // SubscriptionManager.INVALID_SUBSCRIPTION_ID
  public TelephonyManager createForSubscriptionId(int subId) { return new TelephonyManager(); }
  // getActiveModemCount: a device that is neither voice, SMS nor data capable has no modem.
  public int getPhoneCount() { return 0; }
  public int getActiveModemCount() { return 0; }
  public boolean isVoiceCapable() { return false; }
  public boolean isSmsCapable() { return false; }
  public boolean hasCarrierPrivileges() { return false; }
  public void registerTelephonyCallback(java.util.concurrent.Executor executor, TelephonyCallback callback) {
    java.util.Objects.requireNonNull(executor);
    java.util.Objects.requireNonNull(callback);
  }
  public void unregisterTelephonyCallback(TelephonyCallback callback) {}
  public void listen(PhoneStateListener l, int events) {
    if ((events & 32) != 0)
      l.onCallStateChanged(0, "");
  }
}
