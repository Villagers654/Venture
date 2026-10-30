package com.venture.runtime;

import android.app.Application;
import android.content.pm.*;
import android.content.res.*;
import android.util.DisplayMetrics;
import java.io.File;
import java.util.Objects;

public final class RuntimeEnvironment {
  private static volatile Host host = new NativeHost();
  private static volatile RuntimeEnvironment current;
  public final PackageModel manifest;
  public final File apk, data, cache, obb, nativeLib, external;
  private final String guestNativeLib;
  public final AssetManager assets = new AssetManager();
  public final Resources resources;
  public final PackageManager packageManager;
  public ClassLoader loader;
  public volatile Application application;
  public final java.util.concurrent.ConcurrentMap<String, android.content.ContentProvider> contentProviders =
      new java.util.concurrent.ConcurrentHashMap<>();
  private static volatile long displayPeriod = Long.getLong("venture.displayPeriodNs", 11_111_111L);
  public volatile float refreshRate = (float)(1_000_000_000d / displayPeriod);
  public static void setDisplayPeriod(long period) {
    if (period <= 0) throw new IllegalArgumentException("Invalid display period");
    boolean changed = displayPeriod != period;
    displayPeriod = period;
    RuntimeEnvironment e = current;
    if (e != null) e.refreshRate = (float)(1_000_000_000d / period);
    if (changed) android.view.Choreographer.displayPeriodChanged(period);
  }
  public volatile boolean hostVsync;
  private RuntimeEnvironment(String[] args) {
    if (args.length < 8)
      throw new IllegalArgumentException(
          "Expected package, APK, data, cache, OBB, native libs, SDK, converted jar");
    for (int i = 0; i < 8; ++i)
      if (args[i] == null || args[i].isBlank())
        throw new IllegalArgumentException("Empty launch argument " + i);
    apk = new File(args[1]).getAbsoluteFile();
    data = new File(args[2]).getAbsoluteFile();
    cache = new File(args[3]).getAbsoluteFile();
    obb = new File(args[4]).getAbsoluteFile();
    nativeLib = new File(args[5]).getAbsoluteFile();
    external = new File(data, "external-data/files");
    android.os.Build.VERSION.SDK_INT = Integer.parseInt(args[6]);
    android.os.Build.VERSION.SDK = args[6];
    manifest = new PackageModel(
        Objects.requireNonNull(host.readAsset("AndroidManifest.xml"), "Missing manifest"), host.readAsset("resources.arsc"));
    if (!manifest.info.packageName.equals(args[0]))
      throw new IllegalArgumentException("APK package differs from ProcessConfig");
    guestNativeLib = "/data/app/" + args[0] +
        (android.os.Build.CPU_ABI.equals("armeabi-v7a") ? "/lib/arm" : "/lib/arm64");
    ApplicationInfo info = manifest.info.applicationInfo;
    info.sourceDir = apk.toString();
    info.publicSourceDir = info.sourceDir;
    info.dataDir = data.toString();
    info.nativeLibraryDir = nativeLib.toString();
    resources = new Resources(assets, new DisplayMetrics(), new Configuration());
    packageManager = new PackageManager(manifest);
    // A native-only APK (no classes.dex) passes the APK itself: it has no classes
    // to load, and its NativeActivity comes from the framework.
    loader = new dalvik.system.PathClassLoader(args[7].equals(args[1]) ? "" : args[7],
        guestNativeLibDir(), RuntimeEnvironment.class.getClassLoader());
  }
  public static synchronized void configure(String[] args, Host bridge) {
    if (current != null)
      throw new IllegalStateException("One application per runtime");
    host = Objects.requireNonNull(bridge);
    current = new RuntimeEnvironment(args);
  }
  public static void configure(String[] args) {
    configure(args, new NativeHost());
  }
  public static void configureDisplay(
      int width, int height, int density, float refresh, boolean vsync) {
    if (width <= 0 || height <= 0 || density <= 0 || !Float.isFinite(refresh) || refresh <= 0)
      throw new IllegalArgumentException("Invalid display profile");
    Runnable update = () -> {
      RuntimeEnvironment e = get();
      var m = e.resources.getDisplayMetrics();
      m.widthPixels = width;
      m.heightPixels = height;
      m.densityDpi = density;
      m.density = density / 160f;
      m.scaledDensity = m.density * e.resources.getConfiguration().fontScale;
      m.xdpi = m.ydpi = density;
      e.refreshRate = refresh;
      e.hostVsync = vsync;
      var c = e.resources.getConfiguration();
      c.densityDpi = density;
      c.screenWidthDp = Math.round(width / m.density);
      c.screenHeightDp = Math.round(height / m.density);
      c.smallestScreenWidthDp = Math.min(c.screenWidthDp, c.screenHeightDp);
      c.orientation = width >= height ? 2 : 1;
      android.hardware.display.DisplayManager.ventureDisplayChanged();
    };
    var main = android.os.Looper.getMainLooper();
    if (main == null)
      throw new IllegalStateException("Android main Looper is not ready");
    if (main.isCurrentThread())
      update.run();
    else
      new android.os.Handler(main).post(update);
  }
  public static Host host() {
    return host;
  }
  public static RuntimeEnvironment get() {
    RuntimeEnvironment e = current;
    if (e == null)
      throw new IllegalStateException("Framework not configured");
    return e;
  }
  public static Resources resources() {
    return get().resources;
  }
  public static File directory(File f) {
    if (!f.isDirectory() && !f.mkdirs())
      throw new IllegalStateException("Cannot create application directory " + f);
    return f;
  }
  public static String toGuestPath(String path) {
    RuntimeEnvironment e = get();
    return map(path, new File[] {e.apk, e.nativeLib, e.obb, new File(e.data, "external-data"), e.cache, new File(e.data, "external"), new File(e.data, "volumes/data"), new File(e.data, "volumes/system"), e.data},
        new String[] {"/data/app/" + e.manifest.info.packageName + "/base.apk",
            e.guestNativeLibDir(),
            "/storage/emulated/0/Android/obb/" + e.manifest.info.packageName,
            "/storage/emulated/0/Android/data/" + e.manifest.info.packageName,
            "/data/user/0/" + e.manifest.info.packageName + "/cache",
            "/storage/emulated/0",
            "/data",
            "/system",
            "/data/user/0/" + e.manifest.info.packageName});
  }
  /**
   * The host path of an Android absolute path under one of this application's mounts
   * (/storage/emulated/0/..., /data/user/0/<pkg>/..., ...), or the path unchanged. Apps hardcode
   * these or receive them from native code, while java.io here opens host paths.
   */
  public static String toHostPath(String path) {
    if (path == null || !path.startsWith("/") || path.startsWith("//")) return path;
    RuntimeEnvironment e = current;
    if (e == null) return path;
    String pkg = e.manifest.info.packageName;
    String[] guests = {"/storage/emulated/0/Android/obb/" + pkg, "/storage/emulated/0/Android/data/" + pkg,
        "/data/user/0/" + pkg + "/cache", "/data/data/" + pkg + "/cache", "/data/user/0/" + pkg, "/data/data/" + pkg,
        "/sdcard/Android/obb/" + pkg, "/sdcard/Android/data/" + pkg, "/storage/emulated/0", "/sdcard"};
    File[] hosts = {e.obb, new File(e.data, "external-data"), e.cache, e.cache, e.data, e.data,
        e.obb, new File(e.data, "external-data"), new File(e.data, "external"), new File(e.data, "external")};
    for (int i = 0; i < guests.length; i++) {
      String root = guests[i];
      if (path.startsWith(root) && (path.length() == root.length() || path.charAt(root.length()) == '/'))
        return hosts[i].getPath() + hostName(path.substring(root.length())).replace('/', File.separatorChar);
    }
    return path;
  }
  /**
   * An Android file name (or '/'-separated relative path) as a host one: on Windows the
   * characters NTFS forbids (:*?"<>| and controls) become U+F000+c, as Cygwin, MSYS and WSL
   * store them, and as native code does (paths::host_name). Firebase names its preferences
   * "com.google.firebase.common.prefs:<id>". Unchanged on other hosts.
   */
  public static String hostName(String name) {
    if (name == null || File.separatorChar != '\\') return name;
    StringBuilder out = null;
    for (int i = 0; i < name.length(); i++) {
      char c = name.charAt(i);
      boolean forbidden = (c > 0 && c < 0x20) || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
      if (forbidden && out == null) out = new StringBuilder(name.length()).append(name, 0, i);
      if (out != null) out.append(forbidden ? (char) (0xF000 + c) : c);
    }
    return out == null ? name : out.toString();
  }
  public String guestNativeLibDir() {
    return guestNativeLib;
  }
  private static String map(String path, File[] roots, String[] targets) {
    java.nio.file.Path p = new File(path).toPath().toAbsolutePath().normalize();
    for (int i = 0; i < roots.length; i++) {
      var root = roots[i].toPath().toAbsolutePath().normalize();
      if (p.startsWith(root)) {
        if (i == 0 && !p.equals(root))
          continue; // APK is a file, not a mount directory.
        String tail = root.relativize(p).toString().replace('\\', '/');
        return targets[i] + (tail.isEmpty() ? "" : "/" + tail);
      }
    }
    throw new IllegalArgumentException("Path is outside this application");
  }
  public static void trace(String operation) {
    if (Boolean.getBoolean("venture.trace.framework"))
      host.log(3, "Framework", operation);
  }
}
