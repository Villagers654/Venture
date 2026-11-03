package android.nfc;

/** NFC entry point. A Quest has no NFC hardware, so there is no default adapter. */
public final class NfcAdapter {
  private NfcAdapter() {}

  public static NfcAdapter getDefaultAdapter(android.content.Context context) {
    return null;
  }

  public boolean isEnabled() {
    return false;
  }
}
