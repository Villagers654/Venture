package android.telephony;

/** AOSP android.telephony.TelephonyCallback (API 31). Venture has no radio, so no callback fires. */
public class TelephonyCallback {
  public TelephonyCallback() {}
  public interface DisplayInfoListener {
    void onDisplayInfoChanged(TelephonyDisplayInfo telephonyDisplayInfo);
  }
}
