package com.venture.runtime.layoutlib;

import android.content.res.Configuration;
import android.content.res.Resources;
import android.util.DisplayMetrics;
import android.view.Display;
import android.view.DisplayCutout;
import android.view.DisplayInfo;
import android.view.InputDevice;
import com.android.ide.common.rendering.api.ILayoutLog;
import com.venture.runtime.RuntimeEnvironment;
import java.io.File;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * Starts layoutlib's native runtime in a 2D process (docs/design/layoutlib.md):
 * Skia, HWUI, Minikin and libandroidfw in libandroid_runtime, its fonts, ICU
 * data and keyboard maps, then the framework resources. It also owns the 2D
 * panel's display: one window-sized display, as Horizon OS gives a 2D app.
 */
public final class Layoutlib {
  private Layoutlib() {}

  // android.util.Log's natives in libandroid_runtime write here: the host's log.
  // Outside Venture's runtime (the 2D framework's tests) there is no host log.
  private static volatile boolean hostLog = true;
  public static final ILayoutLog LOG = (priority, tag, message) -> {
    String name = tag == null ? "layoutlib" : tag;
    if (hostLog) {
      try {
        RuntimeEnvironment.host().log(Math.max(2, Math.min(7, priority)), name, String.valueOf(message));
        return;
      } catch (UnsatisfiedLinkError noHost) {
        hostLog = false;
      } catch (Throwable t) {
        t.printStackTrace();
      }
    }
    System.err.println(name + ": " + message);
  };

  private static final Map<String, String> properties = new LinkedHashMap<>();
  private static File root;
  private static boolean started;
  private static volatile InputDevice[] inputDevices = new InputDevice[0];
  private static Display display;

  // The panel: a landscape tablet-sized window (1067x667 dp at 240 dpi).
  public static final int PANEL_WIDTH = Integer.getInteger("venture.panel.width", 1600);
  public static final int PANEL_HEIGHT = Integer.getInteger("venture.panel.height", 1000);
  public static final int PANEL_DENSITY = Integer.getInteger("venture.panel.density", 240);

  public static File root() {
    if (root == null) {
      String configured = System.getProperty("venture.layoutlib");
      if (configured != null) {
        root = new File(configured);
      } else {
        try {
          File jar = new File(Layoutlib.class.getProtectionDomain().getCodeSource().getLocation().toURI());
          root = new File(jar.getParentFile(), "layoutlib");
        } catch (java.net.URISyntaxException | RuntimeException e) {
          throw new IllegalStateException("Cannot find layoutlib", e);
        }
      }
    }
    return root;
  }

  public static String frameworkResources() { return new File(root(), "framework-res.apk").getPath(); }

  public static Map<String, String> properties() { return properties; }

  public static String property(String name) { return properties.get(name); }

  public static void setInputDevices(InputDevice[] devices) { inputDevices = devices; }

  public static InputDevice[] inputDevices() { return inputDevices; }

  private static String path(String relative) {
    return new File(root(), relative).getPath().replace('\\', '/');
  }

  /** Loads the natives and fonts. Runs once, before any framework class needs them. */
  public static synchronized void start() {
    if (started) return;
    started = true;
    long begin = System.nanoTime();
    String data = path("data");
    // The framework build's own properties (layoutlib's build.prop), then the
    // device Venture reports, as Build does.
    java.util.Properties build = new java.util.Properties();
    try (var in = new java.io.FileInputStream(new File(root(), "build.prop"))) {
      build.load(in);
    } catch (java.io.IOException e) {
      throw new IllegalStateException("layoutlib's build.prop is missing", e);
    }
    for (String name : build.stringPropertyNames()) properties.put(name, build.getProperty(name));
    properties.put("ro.icu.data.path", findIcuData(new File(root(), "data/icu")));
    properties.put("ro.hyphen.data.dir", data + "/hyphen-data/");
    properties.put("ro.keyboard.paths", data + "/keyboards/Generic.kcm");
    // AOSP's Build reads these (its static initializer needs the ABI lists).
    properties.put("ro.build.version.sdk", String.valueOf(android.os.Build.VERSION.SDK_INT));
    properties.put("ro.build.version.sdk_full", android.os.Build.VERSION.SDK_INT + ".0");
    properties.put("ro.build.version.release", android.os.Build.VERSION.RELEASE);
    properties.put("ro.build.version.release_or_codename", android.os.Build.VERSION.RELEASE);
    properties.put("ro.build.version.release_or_preview_display", android.os.Build.VERSION.RELEASE);
    properties.put("ro.build.version.codename", "REL");
    properties.put("ro.build.type", "user");
    properties.put("ro.build.tags", "release-keys");
    properties.put("ro.product.cpu.abilist", "arm64-v8a,armeabi-v7a,armeabi");
    properties.put("ro.product.cpu.abilist64", "arm64-v8a");
    properties.put("ro.product.cpu.abilist32", "armeabi-v7a,armeabi");
    properties.put("ro.product.manufacturer", android.os.Build.MANUFACTURER);
    properties.put("ro.product.model", android.os.Build.MODEL);
    properties.put("ro.product.brand", android.os.Build.BRAND);
    properties.put("ro.product.device", android.os.Build.DEVICE);
    properties.put("ro.product.name", android.os.Build.PRODUCT);
    // Choreographer paces frames with Looper messages; Venture's host frames
    // drive the 2D panel's presentation.
    properties.put("debug.choreographer.vsync", "false");

    Class<?> config;
    try {
      config = Class.forName("com.android.tools.layoutlib.create.NativeConfig");
      List<String> core = nativeClasses(config, "CORE_CLASS_NATIVES");
      // Layoutlib's own resources come from Android Studio; an app's from its
      // APK, through libandroidfw (the "native" rules of layoutlib-rules.txt).
      core.addAll(List.of("android.content.res.AssetManager", "android.content.res.ApkAssets"));
      System.setProperty("core_native_classes", String.join(",", core));
      System.setProperty("graphics_native_classes", String.join(",", nativeClasses(config, "GRAPHICS_CLASS_NATIVES")));
      System.setProperty("use_base_native_hostruntime", "false");
    } catch (ReflectiveOperationException e) {
      throw new IllegalStateException("layoutlib's NativeConfig is missing", e);
    }
    String os = System.getProperty("os.name").toLowerCase(java.util.Locale.ROOT).startsWith("windows") ? "win" : "linux";
    String suffix = os.equals("win") ? ".dll" : ".so";
    System.load(new File(root(), "lib/libandroid_runtime" + suffix).getAbsolutePath());
    System.load(new File(root(), "lib/layoutlib_jni" + suffix).getAbsolutePath());
    try {
      Class.forName("android.graphics.fonts.SystemFonts_Delegate").getMethod("setFontLocation", String.class)
          .invoke(null, data + "/fonts/");
      Class.forName("libcore.io.MemoryMappedFile_Delegate").getMethod("setDataDir", File.class)
          .invoke(null, new File(root(), "data"));
      for (String name : (String[]) config.getField("DEFERRED_STATIC_INITIALIZER_CLASSES").get(null)) {
        Method initializer;
        try {
          initializer = Class.forName(name).getDeclaredMethod("deferredStaticInitializer");
        } catch (NoSuchMethodException venturesOwn) {
          continue;  // a class Venture supplies (ImageReader) has no deferred initializer
        }
        initializer.setAccessible(true);
        initializer.invoke(null);
      }
      Method fonts = android.graphics.Typeface.class.getDeclaredMethod("loadPreinstalledSystemFontMap");
      fonts.setAccessible(true);
      fonts.invoke(null);
      Method hyphenator = Class.forName("android.text.Hyphenator").getDeclaredMethod("init");
      hyphenator.setAccessible(true);
      hyphenator.invoke(null);
    } catch (ReflectiveOperationException e) {
      throw new IllegalStateException("layoutlib did not start", e);
    }
    SystemServices.install();
    RuntimeEnvironment.trace(String.format("layoutlib started in %.0f ms", (System.nanoTime() - begin) / 1e6));
  }

  private static String findIcuData(File directory) {
    File[] files = directory.listFiles((dir, name) -> name.startsWith("icudt") && name.endsWith(".dat"));
    if (files == null || files.length == 0) throw new IllegalStateException("layoutlib's ICU data is missing");
    return files[0].getPath().replace('\\', '/');
  }

  // The classes whose natives libandroid_runtime registers, less those Venture
  // supplies itself (their natives are Venture's). Some names are not classes
  // but native setup steps ("android.graphics.Graphics" caches the field IDs
  // other graphics natives use; without it they write through ID 0, into object
  // headers), so a name without a class stays.
  private static List<String> nativeClasses(Class<?> config, String field) throws ReflectiveOperationException {
    List<String> result = new ArrayList<>();
    for (String name : (String[]) config.getField(field).get(null)) {
      try {
        if (isVentures(Class.forName(name, false, Layoutlib.class.getClassLoader()))) continue;
      } catch (ClassNotFoundException setupStep) {
        // registered by name
      }
      result.add(name);
    }
    return result;
  }

  private static boolean isVentures(Class<?> type) {
    var source = type.getProtectionDomain().getCodeSource();
    return source != null && source.getLocation().getPath().endsWith("venture-framework-2d.jar");
  }

  /** The panel's display metrics and configuration, applied to the app's resources. */
  public static void configureResources(Resources resources) {
    DisplayMetrics metrics = resources.getDisplayMetrics();
    metrics.setToDefaults();
    metrics.widthPixels = PANEL_WIDTH;
    metrics.heightPixels = PANEL_HEIGHT;
    // CompatibilityInfo.applyToDisplayMetrics, which every configuration update
    // runs, sets the size from these, as DisplayManager fills them on Android.
    metrics.noncompatWidthPixels = PANEL_WIDTH;
    metrics.noncompatHeightPixels = PANEL_HEIGHT;
    metrics.densityDpi = PANEL_DENSITY;
    metrics.density = PANEL_DENSITY / 160f;
    metrics.scaledDensity = metrics.density;
    metrics.xdpi = metrics.ydpi = PANEL_DENSITY;
    Configuration config = new Configuration(resources.getConfiguration());
    config.setToDefaults();
    config.densityDpi = PANEL_DENSITY;
    config.screenWidthDp = Math.round(PANEL_WIDTH / metrics.density);
    config.screenHeightDp = Math.round(PANEL_HEIGHT / metrics.density);
    config.smallestScreenWidthDp = Math.min(config.screenWidthDp, config.screenHeightDp);
    config.orientation = PANEL_WIDTH >= PANEL_HEIGHT ? Configuration.ORIENTATION_LANDSCAPE : Configuration.ORIENTATION_PORTRAIT;
    config.screenLayout = Configuration.SCREENLAYOUT_SIZE_LARGE | Configuration.SCREENLAYOUT_LONG_NO
        | Configuration.SCREENLAYOUT_LAYOUTDIR_LTR;
    config.touchscreen = Configuration.TOUCHSCREEN_FINGER;
    config.keyboard = Configuration.KEYBOARD_QWERTY;
    config.navigation = Configuration.NAVIGATION_NONAV;
    config.uiMode = Configuration.UI_MODE_TYPE_NORMAL | Configuration.UI_MODE_NIGHT_YES;
    config.setLocales(android.os.LocaleList.getDefault());
    config.windowConfiguration.setBounds(new android.graphics.Rect(0, 0, PANEL_WIDTH, PANEL_HEIGHT));
    config.windowConfiguration.setAppBounds(0, 0, PANEL_WIDTH, PANEL_HEIGHT);
    config.windowConfiguration.setMaxBounds(0, 0, PANEL_WIDTH, PANEL_HEIGHT);
    resources.updateConfiguration(config, metrics);
    // AdaptiveIconDrawable takes its mask from layoutlib's delegate, which
    // Android Studio sets to the previewed device's; a device's is the framework's
    // config_icon_mask. Without one, every adaptive icon (the default activity
    // icon a window's decor loads included) fails to inflate.
    if (android.graphics.drawable.AdaptiveIconDrawable_Delegate.sPath == null)
      android.graphics.drawable.AdaptiveIconDrawable_Delegate.sPath =
          resources.getString(com.android.internal.R.string.config_icon_mask);
  }

  /** The panel's display. */
  public static synchronized Display display() {
    if (display == null) {
      DisplayInfo info = new DisplayInfo();
      info.logicalWidth = info.appWidth = PANEL_WIDTH;
      info.logicalHeight = info.appHeight = PANEL_HEIGHT;
      info.logicalDensityDpi = PANEL_DENSITY;
      info.physicalXDpi = info.physicalYDpi = PANEL_DENSITY;
      float refresh = RuntimeEnvironment.current() != null ? RuntimeEnvironment.get().refreshRate : 72f;
      info.supportedModes = new Display.Mode[] {new Display.Mode(1, PANEL_WIDTH, PANEL_HEIGHT, refresh)};
      info.modeId = info.defaultModeId = 1;
      info.displayCutout = DisplayCutout.NO_CUTOUT;
      info.name = "Venture panel";
      info.state = Display.STATE_ON;
      display = new Display(null, Display.DEFAULT_DISPLAY, info, RuntimeEnvironment.resources());
    }
    return display;
  }

  /** A virtual display an app made (DisplayManager.createVirtualDisplay), as DisplayManagerService describes it. */
  public static Display virtualDisplay(int id, String name, int width, int height, int densityDpi, int flags) {
    DisplayInfo info = new DisplayInfo();
    info.type = Display.TYPE_VIRTUAL;
    info.logicalWidth = info.appWidth = width;
    info.logicalHeight = info.appHeight = height;
    info.logicalDensityDpi = densityDpi;
    info.physicalXDpi = info.physicalYDpi = densityDpi;
    float refresh = RuntimeEnvironment.current() != null ? RuntimeEnvironment.get().refreshRate : 72f;
    info.supportedModes = new Display.Mode[] {new Display.Mode(1, width, height, refresh)};
    info.modeId = info.defaultModeId = 1;
    info.displayCutout = DisplayCutout.NO_CUTOUT;
    info.name = name;
    info.state = Display.STATE_ON;
    // DisplayManager.VIRTUAL_DISPLAY_FLAG_PRESENTATION and _SECURE.
    if ((flags & 2) != 0) info.flags |= Display.FLAG_PRESENTATION;
    if ((flags & 4) != 0) info.flags |= Display.FLAG_SECURE;
    return new Display(null, id, info, RuntimeEnvironment.resources());
  }

  /** Surface.release of a Surface holding a Venture window (tools/layoutlib/ProcessLayoutlib window-handle). */
  public static void releaseWindow(long handle) {
    if (handle != 0) RuntimeEnvironment.host().releaseSurface(handle);
  }
}
