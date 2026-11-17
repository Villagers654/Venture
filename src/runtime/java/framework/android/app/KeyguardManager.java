package android.app;

/** A Quest as Venture presents it has no lock screen credential and is never locked while a game runs. */
public class KeyguardManager {
  public boolean isDeviceSecure() { return false; }
  public boolean isKeyguardSecure() { return false; }
  public boolean isKeyguardLocked() { return false; }
  public boolean isDeviceLocked() { return false; }
  public boolean inKeyguardRestrictedInputMode() { return false; }
}
