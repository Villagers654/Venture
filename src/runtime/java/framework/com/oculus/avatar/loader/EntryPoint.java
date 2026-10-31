package com.oculus.avatar.loader;

import android.content.Context;

/**
 * Horizon's entry point for the Avatar SDK 1 driver, which games' avatar loaders
 * (libovravatarloader.so) find through createPackageContext("com.oculus.horizon")
 * and call with SDK version numbers (five ints in SDK 20; older loaders may pass
 * fewer). Each returns the driver's function lookup. The driver here answers
 * every call as if no avatar were available; without one the loader exits the
 * game (First Steps).
 */
public final class EntryPoint {
  private EntryPoint() {}
  private static long driver() { return com.venture.runtime.NativeHost.avatarDriver(); }
  public static long loadHorizon64(Context app, Context horizon, int a0, int a1, int a2) { return driver(); }
  public static long loadHorizon64(Context app, Context horizon, int a0, int a1, int a2, int a3) { return driver(); }
  public static long loadHorizon64(Context app, Context horizon, int a0, int a1, int a2, int a3, int a4) { return driver(); }
  public static long loadHorizon64(Context app, Context horizon, int a0, int a1, int a2, int a3, int a4, int a5) { return driver(); }
  public static long loadHorizon64(Context app, Context horizon, int a0, int a1, int a2, int a3, int a4, int a5, int a6) { return driver(); }
  public static long loadHorizon(Context app, Context horizon, int a0, int a1, int a2) { return driver(); }
  public static long loadHorizon(Context app, Context horizon, int a0, int a1, int a2, int a3) { return driver(); }
  public static long loadHorizon(Context app, Context horizon, int a0, int a1, int a2, int a3, int a4) { return driver(); }
  public static long loadHorizon(Context app, Context horizon, int a0, int a1, int a2, int a3, int a4, int a5) { return driver(); }
  public static long loadHorizon(Context app, Context horizon, int a0, int a1, int a2, int a3, int a4, int a5, int a6) { return driver(); }
  public static long loadGeneric(Context app, Context horizon, int a0, int a1, int a2) { return driver(); }
  public static long loadGeneric(Context app, Context horizon, int a0, int a1, int a2, int a3) { return driver(); }
  public static long loadGeneric(Context app, Context horizon, int a0, int a1, int a2, int a3, int a4) { return driver(); }
  public static long loadGeneric(Context app, Context horizon, int a0, int a1, int a2, int a3, int a4, int a5) { return driver(); }
  public static long loadGeneric(Context app, Context horizon, int a0, int a1, int a2, int a3, int a4, int a5, int a6) { return driver(); }
}
