package com.oculus.systemdriver;

/**
 * The Quest system driver's OpenXR entry point (VrDriver.apk). Meta's Android
 * OpenXR loader, under whatever file name a game bundles it, finds the runtime
 * through createPackageContext("com.oculus.systemdriver", CONTEXT_INCLUDE_CODE |
 * CONTEXT_IGNORE_SECURITY): it loads this class with that context's class loader,
 * calls loadXrRuntime(), then resolves xrInitializeLoaderKHR and
 * xrNegotiateLoaderRuntimeInterface through getProcAddr. Here the runtime is
 * Venture's OpenXR runtime, which is always resident; getProcAddr returns the
 * guest address of its export, or 0 for a name the runtime does not export.
 */
public final class DriverLoader {
  private DriverLoader() {}
  public static void loadXrRuntime() {}
  public static long getProcAddr(String name) {
    return name == null ? 0 : com.venture.runtime.NativeHost.openxrRuntimeProcAddr(name);
  }
}
