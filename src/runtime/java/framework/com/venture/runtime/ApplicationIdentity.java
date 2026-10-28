package com.venture.runtime;

import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.HashSet;
import java.util.function.IntFunction;
import java.util.zip.ZipFile;

/** Manifest application label, without loading any application classes. */
public final class ApplicationIdentity {
  public static String label(BinaryResources.Node manifest, IntFunction<BinaryResources.Value> resources) {
    String fallback = manifest.text("package", "");
    var application = manifest.child("application");
    var value = application == null ? null : application.attributes.get("label");
    var seen = new HashSet<Integer>();
    while (value != null && value.type() == 1) {
      if (value.data() == 0) return fallback;
      if (!seen.add(value.data())) throw new IllegalArgumentException("Cyclic application label");
      value = resources.apply(value.data());
    }
    return value != null && value.type() == 3 && value.text() != null && !value.text().isBlank()
        ? value.text() : fallback;
  }
  public static byte[] apkLabel(String path) throws Exception {
    try (var zip = new ZipFile(Path.of(path).toFile())) {
      byte[] manifest;
      try (var input = zip.getInputStream(zip.getEntry("AndroidManifest.xml"))) {
        manifest = input.readAllBytes();
      }
      var resource = zip.getEntry("resources.arsc");
      byte[] data = null;
      if (resource != null) try (var input = zip.getInputStream(resource)) { data = input.readAllBytes(); }
      var table = new ResourceTable(data);
      return label(BinaryResources.xml(manifest), id -> table.resolve(id,
          new android.content.res.Configuration(), new android.util.DisplayMetrics())).getBytes(StandardCharsets.UTF_8);
    }
  }
}
