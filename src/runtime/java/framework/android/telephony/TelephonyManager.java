package android.telephony;
public class TelephonyManager {
  public int getPhoneType() {
    return 0;
  }
  public int getCallState() {
    return 0;
  }
  public int getNetworkType() {
    return 0;
  }
  public String getNetworkOperator() {
    return "";
  }
  public String getNetworkCountryIso() {
    return "";
  }
  public String getSimCountryIso() {
    return "";
  }
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
