import android.os.Build;
import java.util.Arrays;

/** Each profile runs in a fresh JVM, just as each guest launch does. */
public final class AbiProfileTest {
  public static void main(String[] args) {
    String abi = args[0];
    boolean arm32 = abi.equals("armeabi-v7a");
    if (!Build.CPU_ABI.equals(abi) || !Arrays.equals(Build.SUPPORTED_ABIS, new String[] {abi}) ||
        !Arrays.equals(Build.SUPPORTED_32_BIT_ABIS, arm32 ? new String[] {abi} : new String[] {}) ||
        !Arrays.equals(Build.SUPPORTED_64_BIT_ABIS, arm32 ? new String[] {} : new String[] {abi}))
      throw new AssertionError("Guest ABI profile differs from immutable launch selection");
    System.out.println("Build ABI profile: " + abi);
  }
}
