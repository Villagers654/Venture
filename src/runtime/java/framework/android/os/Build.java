package android.os;
public final class Build {
  public static final String UNKNOWN = "unknown", SOC_MODEL = UNKNOWN, SOC_MANUFACTURER = UNKNOWN;
  // The native launch-time identity is installed before this class initializes.
  // These are not compile-time constants: callers must observe the selected APK profile.
  public static final String MANUFACTURER = "Oculus",
                             MODEL = System.getProperty("venture.device.model", UNKNOWN),
                             DEVICE = System.getProperty("venture.device.name", UNKNOWN),
                             BRAND = "oculus", PRODUCT = DEVICE, HARDWARE = DEVICE,
                             TYPE = "user", TAGS = "release-keys";
  public static final String FINGERPRINT = "oculus/" + DEVICE + "/" + DEVICE + ":12/Venture/0:user/release-keys";
  public static final String CPU_ABI = System.getProperty("venture.guest.abi", "arm64-v8a"), CPU_ABI2 = "", BOARD = DEVICE,
                             HOST = "venture", ID = "Venture", DISPLAY = ID, SERIAL = "unknown";
  public static final String[] SUPPORTED_ABIS = {CPU_ABI},
                               SUPPORTED_64_BIT_ABIS = CPU_ABI.equals("arm64-v8a") ? new String[] {CPU_ABI} : new String[] {},
                               SUPPORTED_32_BIT_ABIS = CPU_ABI.equals("armeabi-v7a") ? new String[] {CPU_ABI} : new String[] {};
  public static class VERSION {
    public static int SDK_INT = 32;
    public static String SDK = "32", RELEASE = "12", CODENAME = "REL", INCREMENTAL = "1";
  }
  public static class VERSION_CODES {
    public static final int LOLLIPOP = 21, M = 23, N = 24, N_MR1 = 25, O = 26, O_MR1 = 27, P = 28,
                            Q = 29, R = 30, S = 31, S_V2 = 32, TIRAMISU = 33;
  }
}
