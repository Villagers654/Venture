package com.venture.runtime;

import android.content.pm.*;
import android.content.*;
import android.os.Bundle;
import java.util.*;

/**
 * Installed APK metadata, parsed from its original binary AndroidManifest.xml.
 */
public final class PackageModel {
  public final PackageInfo info = new PackageInfo();
  public final List<ActivityInfo> activities = new ArrayList<>();
  public record ActivityFilter(ActivityInfo activity, Set<String> actions, Set<String> categories,
                               boolean hasData, int priority) {}
  public final List<ActivityFilter> activityFilters = new ArrayList<>();
  private final Map<ActivityFilter, IntentFilter> matchers = new IdentityHashMap<>();
  public final List<ProviderInfo> providers = new ArrayList<>();
  public final List<ServiceInfo> services = new ArrayList<>();
  // Manifest receivers, for their component information and enabled state.
  public final List<ActivityInfo> receivers = new ArrayList<>();
  public record ServiceFilter(ServiceInfo service, IntentFilter filter) {}
  public final List<ServiceFilter> serviceFilters = new ArrayList<>();
  public final Set<String> permissions = new HashSet<>();
  public String launcher;
  public PackageModel(byte[] binary) {
    this(BinaryResources.xml(binary));
  }
  public PackageModel(byte[] binary, byte[] resources) {
    this(BinaryResources.xml(binary), new java.util.function.IntFunction<BinaryResources.Value>() {
      private ResourceTable table;
      public BinaryResources.Value apply(int id) {
        if ((id >>> 24) == 1) return SystemAssets.table().nonConfigurationValue(id);
        if (table == null) table = new ResourceTable(resources);
        return table.nonConfigurationValue(id);
      }
    });
  }
  public PackageModel(BinaryResources.Node manifest) {
    this(manifest, id -> { throw new IllegalArgumentException("Manifest resources unavailable"); });
  }
  public PackageModel(BinaryResources.Node manifest, java.util.function.IntFunction<BinaryResources.Value> resources) {
    info.packageName = manifest.text("package", "");
    info.versionCode = manifest.integer("versionCode", 0);
    info.versionName = manifest.text("versionName", "");
    ApplicationInfo app = new ApplicationInfo();
    info.applicationInfo = app;
    app.packageName = info.packageName;
    BinaryResources.Node sdk = manifest.child("uses-sdk");
    app.targetSdkVersion = sdk == null ? 1 : sdk.integer("targetSdkVersion", 1);
    app.minSdkVersion = sdk == null ? 1 : sdk.integer("minSdkVersion", 1);
    for (var n : manifest.children)
      if (n.name.equals("uses-permission"))
        permissions.add(n.text("name", ""));
    BinaryResources.Node application = manifest.child("application");
    if (application == null)
      throw new IllegalArgumentException("Manifest has no application");
    app.className = qualified(application.text("name", "android.app.Application"));
    app.name = app.className;
    app.enabled = application.integer("enabled", 1) != 0;
    app.processName = processName(application.text("process", info.packageName));
    app.metaData = metadata(application);
    app.theme = application.integer("theme", 0);
    app.flags = application.integer("debuggable", 0) != 0 ? ApplicationInfo.FLAG_DEBUGGABLE : 0;
    for (var n : application.children) {
      if (n.name.equals("activity") || n.name.equals("activity-alias")) {
        ActivityInfo a = new ActivityInfo();
        a.packageName = info.packageName;
        a.name = qualified(n.text("name", ""));
        String target = n.text("targetActivity", null);
        a.targetActivity = target == null ? null : qualified(target);
        a.applicationInfo = app;
        a.metaData = metadata(n);
        a.theme = n.integer("theme", app.theme);
        a.screenOrientation = n.integer("screenOrientation", -1);
        a.configChanges = n.integer("configChanges", 0);
        String parent = n.text("parentActivityName", "");
        a.parentActivityName = parent.isEmpty() ? null : qualified(parent);
        // AOSP 12L ParsedActivity.makeAlias copies these target properties;
        // activity-alias has no theme/orientation/configChanges attributes.
        if (n.name.equals("activity-alias")) for (var targetInfo : activities)
          if (targetInfo.targetActivity == null && targetInfo.name.equals(a.targetActivity)) {
            a.theme = targetInfo.theme;
            a.screenOrientation = targetInfo.screenOrientation;
            a.configChanges = targetInfo.configChanges;
            if (a.parentActivityName == null) a.parentActivityName = targetInfo.parentActivityName;
            break;
          }
        a.exported = n.integer("exported", 1) != 0;
        a.enabled = n.integer("enabled", 1) != 0 && application.integer("enabled", 1) != 0;
        activities.add(a);
        for (var filter : n.children)
          if (filter.name.equals("intent-filter")) {
            Set<String> actions = new HashSet<>(), categories = new HashSet<>();
            IntentFilter matcher = new IntentFilter();
            // ComponentResolver.adjustPriority caps non-privileged APKs at zero.
            matcher.setPriority(Math.min(filter.integer("priority", 0), 0));
            boolean hasData = false;
            try {
              for (var child : filter.children) {
                String name = child.text("name", "");
                if (child.name.equals("action")) { actions.add(name); matcher.addAction(name); }
                if (child.name.equals("category")) { categories.add(name); matcher.addCategory(name); }
                hasData |= child.name.equals("data");
                if (child.name.equals("data")) addData(matcher, child, resources);
              }
            } catch (RuntimeException invalid) {
              // Never retain a partially parsed filter: it could match too broadly.
              // Do not log data attributes (deep links can contain credentials).
              android.util.Log.w("PackageModel", "Skipping unresolved intent-filter for " + a.name
                  + " (" + invalid.getClass().getSimpleName() + ")");
              continue;
            }
            var entry = new ActivityFilter(a, Set.copyOf(actions), Set.copyOf(categories), hasData, matcher.getPriority());
            activityFilters.add(entry);
            matchers.put(entry, matcher);
          }
      } else if (n.name.equals("service")) {
        ServiceInfo s = new ServiceInfo();
        s.name = qualified(n.text("name", ""));
        s.packageName = info.packageName;
        s.applicationInfo = app;
        s.metaData = metadata(n);
        s.permission = n.text("permission", null);
        s.processName = processName(n.text("process", app.processName));
        s.enabled = n.integer("enabled", 1) != 0 && app.enabled;
        s.exported = n.integer("exported", 0) != 0;
        s.foregroundServiceType = n.integer("foregroundServiceType", 0);
        services.add(s);
        for (var filter : n.children) {
          if (!filter.name.equals("intent-filter")) continue;
          var matcher = new IntentFilter();
          matcher.setPriority(filter.integer("priority", 0));
          try {
            for (var child : filter.children) {
              if (child.name.equals("action")) matcher.addAction(child.text("name", ""));
              if (child.name.equals("category")) matcher.addCategory(child.text("name", ""));
              if (child.name.equals("data")) addData(matcher, child, resources);
            }
          } catch (RuntimeException invalid) {
            android.util.Log.w("PackageModel", "Skipping unresolved service filter for " + s.name
                + " (" + invalid.getClass().getSimpleName() + ")");
            continue;
          }
          serviceFilters.add(new ServiceFilter(s, matcher));
        }
      } else if (n.name.equals("receiver")) {
        ActivityInfo r = new ActivityInfo();
        r.name = qualified(n.text("name", ""));
        r.packageName = info.packageName;
        r.applicationInfo = app;
        r.metaData = metadata(n);
        r.processName = processName(n.text("process", app.processName));
        r.enabled = n.integer("enabled", 1) != 0 && app.enabled;
        r.exported = n.integer("exported", 0) != 0;
        receivers.add(r);
      } else if (n.name.equals("provider")) {
        ProviderInfo p = new ProviderInfo();
        p.name = qualified(n.text("name", ""));
        p.packageName = info.packageName;
        p.applicationInfo = app;
        p.authority = n.text("authorities", "");
        p.metaData = metadata(n);
        p.initOrder = n.integer("initOrder", 0);
        providers.add(p);
      }
    }
    providers.sort(Comparator.comparingInt((ProviderInfo p) -> p.initOrder).reversed());
    info.activities = activities.toArray(ActivityInfo[] ::new);
    info.providers = providers.toArray(ProviderInfo[] ::new);
    info.services = services.toArray(ServiceInfo[] ::new);
    info.requestedPermissions = permissions.toArray(String[] ::new);
    Intent launch = launchIntent();
    if (launch != null) {
      String name = launch.getComponent().getClassName();
      var activity = activities.stream().filter(a -> a.name.equals(name)).findFirst().orElseThrow();
      launcher = activity.targetActivity == null ? activity.name : activity.targetActivity;
    }
  }
  private static String dataString(BinaryResources.Node data, String name,
      java.util.function.IntFunction<BinaryResources.Value> resources) {
    var value = data.attributes.get(name);
    Set<Integer> seen = new HashSet<>();
    // ParsedIntentInfoUtils uses TypedArray.getNonConfigurationString(..., 0).
    while (value != null && value.type() == 1 && value.data() != 0) {
      if (!seen.add(value.data())) throw new IllegalArgumentException("Cyclic manifest resource");
      value = Objects.requireNonNull(resources.apply(value.data()));
    }
    if (value == null || value.type() == 0 || value.type() == 1) return null;
    if (value.type() == 2) throw new IllegalArgumentException("Unresolved manifest attribute");
    return value.encoded();
  }
  private static void addData(IntentFilter filter, BinaryResources.Node data,
      java.util.function.IntFunction<BinaryResources.Value> resources) {
    String type = dataString(data, "mimeType", resources), scheme = dataString(data, "scheme", resources),
        host = dataString(data, "host", resources);
    try { if (type != null) filter.addDataType(type); }
    catch (IntentFilter.MalformedMimeTypeException invalid) { throw new IllegalArgumentException("Invalid manifest MIME type", invalid); }
    if (scheme != null) filter.addDataScheme(scheme);
    if (host != null) filter.addDataAuthority(host, dataString(data, "port", resources));
    for (var prefix : List.of("path", "ssp")) {
      String[] names = {prefix, prefix + "Prefix", prefix + "Pattern", prefix + "AdvancedPattern", prefix + "Suffix"};
      for (int i = 0; i < names.length; i++) {
        String pattern = dataString(data, names[i], resources);
        if (pattern != null) {
          if (prefix.equals("path")) filter.addDataPath(pattern, i);
          else filter.addDataSchemeSpecificPart(pattern, i);
        }
      }
    }
  }
  public List<ResolveInfo> resolveActivities(Intent intent, int flags) {
    Objects.requireNonNull(intent);
    ComponentName component = intent.getComponent();
    String pkg = component == null ? intent.getPackage() : component.getPackageName();
    if (pkg != null && !info.packageName.equals(pkg)) return List.of();
    if (component != null) {
      for (var activity : activities) if (activity.enabled && activity.name.equals(component.getClassName())) {
        var resolved = new ResolveInfo(); resolved.activityInfo = activity; return List.of(resolved);
      }
      return List.of();
    }
    // AOSP IntentResolver indexes global implicit queries by action, MIME type
    // or scheme. With none of these, no candidate cut exists. A package query
    // explicitly supplies candidates and may match a null action.
    if (pkg == null && intent.getAction() == null && intent.getType() == null && intent.getScheme() == null)
      return List.of();
    String type = intent.resolveType(new ContentResolver());
    var matches = new LinkedHashMap<String, ResolveInfo>();
    for (var entry : activityFilters) {
      if (!entry.activity().enabled) continue;
      IntentFilter filter = matchers.get(entry);
      if (filter == null) { // Retain the original public ActivityFilter construction API.
        if (entry.hasData()) continue;
        filter = new IntentFilter();
        entry.actions().forEach(filter::addAction); entry.categories().forEach(filter::addCategory);
      }
      if ((flags & PackageManager.MATCH_DEFAULT_ONLY) != 0 && !filter.hasCategory("android.intent.category.DEFAULT")) continue;
      int match = filter.match(intent.getAction(), type, intent.getScheme(), intent.getData(), intent.getCategories(), "PackageModel");
      if (match < 0) continue;
      var resolved = new ResolveInfo(); resolved.activityInfo = entry.activity(); resolved.priority = entry.priority();
      resolved.match = match; resolved.isDefault = filter.hasCategory("android.intent.category.DEFAULT");
      var previous = matches.get(entry.activity().name);
      if (previous != null && RESOLVE_ORDER.compare(previous, resolved) <= 0) continue;
      matches.put(entry.activity().name, resolved);
    }
    var results = new ArrayList<>(matches.values());
    results.sort(RESOLVE_ORDER);
    return results;
  }
  public List<ResolveInfo> resolveServices(Intent intent, int flags) {
    Objects.requireNonNull(intent);
    ComponentName component = intent.getComponent();
    String pkg = component == null ? intent.getPackage() : component.getPackageName();
    if (!info.applicationInfo.enabled || pkg != null && !info.packageName.equals(pkg)) return List.of();
    if (component != null) {
      for (var service : services) if (service.enabled && service.name.equals(component.getClassName())) {
        var result = new ResolveInfo(); result.serviceInfo = service; return List.of(result);
      }
      return List.of();
    }
    if (pkg == null && intent.getAction() == null && intent.getType() == null && intent.getScheme() == null)
      return List.of();
    String type = intent.resolveType(new ContentResolver());
    var matches = new LinkedHashMap<String, ResolveInfo>();
    for (var entry : serviceFilters) {
      if (!entry.service().enabled) continue;
      var filter = entry.filter();
      if ((flags & PackageManager.MATCH_DEFAULT_ONLY) != 0 && !filter.hasCategory("android.intent.category.DEFAULT")) continue;
      int match = filter.match(intent.getAction(), type, intent.getScheme(), intent.getData(), intent.getCategories(), "PackageModel");
      if (match < 0) continue;
      var result = new ResolveInfo(); result.serviceInfo = entry.service();
      result.priority = filter.getPriority(); result.match = match;
      result.isDefault = filter.hasCategory("android.intent.category.DEFAULT");
      var previous = matches.get(entry.service().name);
      if (previous == null || RESOLVE_ORDER.compare(previous, result) > 0) matches.put(entry.service().name, result);
    }
    var results = new ArrayList<>(matches.values()); results.sort(RESOLVE_ORDER); return results;
  }
  private static final Comparator<ResolveInfo> RESOLVE_ORDER =
      Comparator.comparingInt((ResolveInfo r) -> r.priority).reversed()
          .thenComparing((ResolveInfo r) -> !r.isDefault)
          .thenComparing(Comparator.comparingInt((ResolveInfo r) -> r.match).reversed());
  public Intent launchIntent() {
    // AOSP PackageManager prefers INFO, then LAUNCHER; Quest additionally has VR.
    for (String category : List.of("android.intent.category.INFO", Intent.CATEGORY_LAUNCHER, "com.oculus.intent.category.VR")) {
      Intent intent = new Intent(Intent.ACTION_MAIN).setPackage(info.packageName).addCategory(category);
      var matches = resolveActivities(intent, 0);
      if (!matches.isEmpty()) return intent.setComponent(new ComponentName(info.packageName, matches.get(0).activityInfo.name))
          .setFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
    }
    return null;
  }
  private String processName(String name) {
    return name.startsWith(":") ? info.packageName + name : name;
  }
  private String qualified(String name) {
    return name.startsWith(".") ? info.packageName + name
        : name.contains(".")    ? name
                                : info.packageName + "." + name;
  }
  private static Bundle metadata(BinaryResources.Node n) {
    Bundle out = new Bundle();
    for (var c : n.children)
      if (c.name.equals("meta-data")) {
        String name = c.text("name", "");
        BinaryResources.Value v = c.attributes.get("value");
        if (v == null)
          v = c.attributes.get("resource");
        if (v != null)
          switch (v.type()) {
            case 3 -> out.putString(name, v.text());
            case 18 -> out.putBoolean(name, v.data() != 0);
            default -> out.putInt(name, v.data());
          }
      }
    return out;
  }
}
