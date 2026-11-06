package com.venture.runtime;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;

/**
 * Runtime ("dangerous") permissions that a Quest grants from its permission dialog. Venture
 * answers that dialog with Allow: the person started the game on their own computer, whose OS
 * keeps its own microphone consent. As on Android, only a permission the manifest declares can be
 * granted, a grant lasts across launches, and an app targeting API 22 or lower holds its declared
 * permissions from install. Other runtime permissions (camera, location, ...) stay denied: a
 * Quest offers apps neither.
 */
public final class RuntimePermissions {
  private RuntimePermissions() {}
  private static final Set<String> GRANTABLE = Set.of("android.permission.RECORD_AUDIO");
  // Per manifest identity; tests build their own PackageModels.
  private static final Map<PackageModel, Set<String>> grants = new WeakHashMap<>();

  public static boolean granted(PackageModel model, String permission) {
    if (!GRANTABLE.contains(permission) || !model.permissions.contains(permission)) return false;
    return model.info.applicationInfo.targetSdkVersion < 23 || grants(model).contains(permission);
  }

  /** The dialog's answer to Activity.requestPermissions from the hosted package. */
  public static void request(String pkg, String[] permissions) {
    RuntimeEnvironment env;
    try { env = RuntimeEnvironment.get(); }
    catch (IllegalStateException unconfigured) { return; }
    if (!env.manifest.info.packageName.equals(pkg)) return;
    request(env.manifest, permissions);
  }
  public static void request(PackageModel model, String[] permissions) {
    List<String> added = new ArrayList<>();
    synchronized (grants) {
      Set<String> set = grants(model);
      for (String permission : permissions)
        if (permission != null && GRANTABLE.contains(permission) && model.permissions.contains(permission)
            && set.add(permission))
          added.add(permission);
      if (added.isEmpty() || !save(model, set)) return;
    }
    android.util.Log.i("PackageManager", "Granted " + String.join(", ", added) + " to " + model.info.packageName);
  }

  private static Set<String> grants(PackageModel model) {
    synchronized (grants) {
      return grants.computeIfAbsent(model, m -> {
        Set<String> set = new HashSet<>();
        Path file = file(m);
        try {
          if (file != null && Files.exists(file))
            for (String line : Files.readAllLines(file, StandardCharsets.UTF_8))
              if (GRANTABLE.contains(line.strip())) set.add(line.strip());
        } catch (IOException e) {
          android.util.Log.w("PackageManager", "Cannot read runtime permission grants: " + e);
        }
        return set;
      });
    }
  }
  // False for a model other than the hosted app's (tests), which keeps its grants in memory.
  private static boolean save(PackageModel model, Set<String> set) {
    Path file = file(model);
    if (file == null) return false;
    try {
      Files.createDirectories(file.getParent());
      Path temp = Files.createTempFile(file.getParent(), "grants-", ".tmp");
      try {
        Files.write(temp, new TreeSet<>(set), StandardCharsets.UTF_8);
        Files.move(temp, file, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
      } finally {
        Files.deleteIfExists(temp);
      }
    } catch (IOException e) {
      android.util.Log.w("PackageManager", "Cannot save runtime permission grants: " + e);
    }
    return true;
  }
  // Beside the user's settings_ssaid.key, outside the app's own data, as Android keeps
  // runtime-permissions.xml in system storage.
  private static Path file(PackageModel model) {
    RuntimeEnvironment env;
    try { env = RuntimeEnvironment.get(); }
    catch (IllegalStateException unconfigured) { return null; }
    if (env.manifest != model) return null;
    java.io.File user = env.data.getParentFile();
    return (user != null ? user : env.data).toPath()
        .resolve("runtime-permissions").resolve(model.info.packageName + ".txt");
  }
}
