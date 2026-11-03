package android.net.wifi;

// AOSP's supplicant states, in declaration order (ordinal() is part of the API surface).
public enum SupplicantState {
  DISCONNECTED, INTERFACE_DISABLED, INACTIVE, SCANNING, AUTHENTICATING, ASSOCIATING, ASSOCIATED,
  FOUR_WAY_HANDSHAKE, GROUP_HANDSHAKE, COMPLETED, DORMANT, UNINITIALIZED, INVALID;
  public static boolean isValidState(SupplicantState state) { return state != UNINITIALIZED && state != INVALID; }
}
