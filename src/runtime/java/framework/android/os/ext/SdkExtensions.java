package android.os.ext;

import android.os.Build;
import android.os.SystemProperties;
import java.util.HashMap;
import java.util.Map;

// AOSP 12L SdkExtensions: the R and S extension levels come from the
// build.version.extensions.* properties that derive_sdk sets from the installed
// mainline modules. Venture has no such modules and sets none, so both are 0,
// and apps keep the code paths that need no extension APIs (photo picker and
// the like). AndroidX BuildCompat reads these on every API 30+ device.
public class SdkExtensions {
  private static final int R_EXTENSION_INT = property("build.version.extensions.r");
  private static final int S_EXTENSION_INT = property("build.version.extensions.s");
  private static final Map<Integer, Integer> ALL_EXTENSION_INTS;
  static {
    var extensions = new HashMap<Integer, Integer>();
    extensions.put(Build.VERSION_CODES.R, R_EXTENSION_INT);
    extensions.put(Build.VERSION_CODES.S, S_EXTENSION_INT);
    ALL_EXTENSION_INTS = Map.copyOf(extensions);
  }

  private SdkExtensions() {}

  public static int getExtensionVersion(int sdk) {
    if (sdk < Build.VERSION_CODES.R)
      throw new IllegalArgumentException(sdk + " does not have extensions");
    if (sdk == Build.VERSION_CODES.R) return R_EXTENSION_INT;
    if (sdk == Build.VERSION_CODES.S) return S_EXTENSION_INT;
    return 0;
  }

  public static Map<Integer, Integer> getAllExtensionVersions() { return ALL_EXTENSION_INTS; }

  // SystemProperties.getInt(key, 0): an absent or malformed value is 0.
  private static int property(String key) {
    try {
      return Integer.parseInt(SystemProperties.get(key));
    } catch (NumberFormatException absent) {
      return 0;
    }
  }
}
