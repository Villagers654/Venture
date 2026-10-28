package android.telephony;
public class PhoneStateListener {
  public static final int LISTEN_NONE = 0, LISTEN_CALL_STATE = 32;
  public void onCallStateChanged(int state, String number) {}
}
