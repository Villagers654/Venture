package com.android.internal.ravenwood;

import com.venture.runtime.layoutlib.ArrayAddresses;

/**
 * AOSP's bridge to its host JVM test runtime (Ravenwood), for a 2D process:
 * the framework otherwise runs as on a device, but the Ravenwood versions of
 * methods that pass array addresses to native code (layoutlib-rules.txt) find
 * their arrays here (com.venture.runtime.layoutlib.ArrayAddresses).
 */
public class RavenwoodHelperBridge {
  public static final String TAG = "RavenwoodHelperBridge";
  private static final RavenwoodHelperBridge sInstance = new RavenwoodHelperBridge();

  public static RavenwoodHelperBridge getInstance() { return sInstance; }

  public boolean isRunningOnRavenwood() { return false; }

  public <T> T fromAddress(long address) { return ArrayAddresses.fromAddress(address); }

  public String getRavenwoodRuntimePath() {
    throw new UnsupportedOperationException("This method can only be used on Ravenwood");
  }

  public static int forExperimentalApiTest() { return 1; }

  public static Throwable getStackTrace(String message) { return new android.app.StackTrace(message); }

  public static long enterMethod(Class<?> type, Object self, String name, String descriptor, Object[] args) { return 0; }

  public static void exitMethod(long token, Class<?> type, Object self, String name, String descriptor, Object result) {}
}
