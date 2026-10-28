package android.system;

import com.venture.runtime.RuntimeEnvironment;
import java.nio.charset.StandardCharsets;
import java.util.Objects;

public final class Os {
  private Os() {}

  // AOSP libcore android/system/Os.java delegates to the process's bionic setenv.
  public static void setenv(String name, String value, boolean overwrite) throws ErrnoException {
    int error = RuntimeEnvironment.host().setEnvironment(
        Objects.requireNonNull(name).getBytes(StandardCharsets.UTF_8),
        Objects.requireNonNull(value).getBytes(StandardCharsets.UTF_8), overwrite);
    if (error != 0) throw new ErrnoException("setenv", error);
  }

  public static String strerror(int error) {
    return RuntimeEnvironment.host().guestErrorString(error);
  }
}
