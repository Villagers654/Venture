package android.os;

import com.venture.runtime.RuntimeEnvironment;
import java.nio.charset.StandardCharsets;
import java.util.Objects;

public final class SystemProperties {
  private SystemProperties() {}
  // AOSP 12L android_os_SystemProperties.cpp native_get: absent or empty
  // values return the supplied default, with null defaults becoming "".
  public static String get(String key) { return get(key, ""); }
  public static String get(String key, String fallback) {
    byte[] value = RuntimeEnvironment.host().systemProperty(Objects.requireNonNull(key));
    return value.length == 0 ? (fallback == null ? "" : fallback)
        : new String(value, StandardCharsets.UTF_8);
  }
}
