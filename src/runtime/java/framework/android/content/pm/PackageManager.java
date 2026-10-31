package android.content.pm;
import android.content.*;
import com.venture.runtime.*;
public class PackageManager {
  public static final String FEATURE_BLUETOOTH = "android.hardware.bluetooth";
  public static final String FEATURE_BLUETOOTH_LE = "android.hardware.bluetooth_le";
  public static final String FEATURE_AUDIO_LOW_LATENCY = "android.hardware.audio.low_latency";
  public static final int GET_ACTIVITIES = 1, GET_INTENT_FILTERS = 32, GET_META_DATA = 128, GET_SIGNATURES = 64,
                          GET_SIGNING_CERTIFICATES = 0x08000000, PERMISSION_GRANTED = 0,
                          PERMISSION_DENIED = -1;
  public static final String FEATURE_VR_MODE_HIGH_PERFORMANCE =
      "android.hardware.vr.high_performance";
  public static final String FEATURE_VULKAN_HARDWARE_LEVEL = "android.hardware.vulkan.level",
      FEATURE_VULKAN_HARDWARE_VERSION = "android.hardware.vulkan.version";
  public static class NameNotFoundException extends Exception {
    public NameNotFoundException() {}
    public NameNotFoundException(String m) {
      super(m);
    }
  }
  private final PackageModel model;
  public PackageManager(PackageModel m) {
    model = m;
  }
  public ApplicationInfo getApplicationInfo(String pkg, int flags) throws NameNotFoundException {
    // Unity's built-in Oculus provider checks this package after reading the
    // application's vr_only/vr_dual metadata. Venture supplies the runtime
    // through libvrapi HLE, so expose that system-runtime package's identity.
    // It has no launchable Activity or bindable service in this framework.
    if ("com.oculus.systemactivities".equals(pkg)) {
      ApplicationInfo runtime = new ApplicationInfo();
      runtime.packageName = pkg;
      runtime.processName = pkg;
      runtime.className = "android.app.Application";
      runtime.name = runtime.className;
      runtime.flags = ApplicationInfo.FLAG_SYSTEM;
      runtime.nativeLibraryDir = "/system/lib64";
      return runtime;
    }
    check(pkg);
    return model.info.applicationInfo;
  }
  public PackageInfo getPackageInfo(String pkg, int flags) throws NameNotFoundException {
    check(pkg);
    if ((flags & (GET_SIGNATURES | GET_SIGNING_CERTIFICATES)) != 0) {
      try {
        model.info.signatures = ApkSignatures.read(RuntimeEnvironment.get().apk);
        model.info.signingInfo = new SigningInfo(model.info.signatures);
      } catch (java.io.IOException e) {
        throw new IllegalStateException("Cannot read APK signer certificates", e);
      }
    }
    return model.info;
  }
  public ProviderInfo getProviderInfo(ComponentName name, int flags) throws NameNotFoundException {
    check(name.getPackageName());
    return model.providers.stream()
        .filter(p -> p.name.equals(name.getClassName()))
        .findFirst()
        .orElseThrow(() -> new NameNotFoundException(name.toString()));
  }
  private void check(String pkg) throws NameNotFoundException {
    if (!model.info.packageName.equals(pkg))
      throw new NameNotFoundException(pkg);
  }
  public ActivityInfo getActivityInfo(ComponentName name, int flags) throws NameNotFoundException {
    check(name.getPackageName());
    return model.activities.stream()
        .filter(a -> a.name.equals(name.getClassName()))
        .findFirst()
        .orElseThrow(() -> new NameNotFoundException(name.toString()));
  }
  public ServiceInfo getServiceInfo(ComponentName name, int flags) throws NameNotFoundException {
    check(name.getPackageName());
    return model.services.stream()
        .filter(s -> s.name.equals(name.getClassName()))
        .findFirst()
        .orElseThrow(() -> new NameNotFoundException(name.toString()));
  }
  public static final int COMPONENT_ENABLED_STATE_DEFAULT = 0, COMPONENT_ENABLED_STATE_ENABLED = 1,
                          COMPONENT_ENABLED_STATE_DISABLED = 2, COMPONENT_ENABLED_STATE_DISABLED_USER = 3,
                          COMPONENT_ENABLED_STATE_DISABLED_UNTIL_USED = 4, DONT_KILL_APP = 1;
  // Overrides made this process; DEFAULT keeps the manifest's own android:enabled.
  private static final java.util.Map<ComponentName, Integer> componentStates = new java.util.concurrent.ConcurrentHashMap<>();
  private static final java.util.Map<ComponentInfo, Boolean> manifestEnabled = new java.util.IdentityHashMap<>();
  public int getComponentEnabledSetting(ComponentName name) {
    if (!model.info.packageName.equals(name.getPackageName()) || component(name) == null)
      throw new IllegalArgumentException("Unknown component: " + name);
    return componentStates.getOrDefault(name, COMPONENT_ENABLED_STATE_DEFAULT);
  }
  public void setComponentEnabledSetting(ComponentName name, int state, int flags) {
    ComponentInfo info = model.info.packageName.equals(name.getPackageName()) ? component(name) : null;
    if (info == null) throw new IllegalArgumentException("Unknown component: " + name);
    if (state < COMPONENT_ENABLED_STATE_DEFAULT || state > COMPONENT_ENABLED_STATE_DISABLED_UNTIL_USED)
      throw new IllegalArgumentException("Invalid new component state: " + state);
    synchronized (manifestEnabled) {
      boolean declared = manifestEnabled.computeIfAbsent(info, i -> i.enabled);
      info.enabled = state == COMPONENT_ENABLED_STATE_DEFAULT ? declared : state == COMPONENT_ENABLED_STATE_ENABLED;
    }
    if (state == COMPONENT_ENABLED_STATE_DEFAULT) componentStates.remove(name);
    else componentStates.put(name, state);
  }
  private ComponentInfo component(ComponentName name) {
    String cls = name.getClassName();
    for (var list : java.util.List.<java.util.List<? extends ComponentInfo>>of(model.activities, model.services,
                                                                               model.providers, model.receivers))
      for (ComponentInfo info : list)
        if (info.name.equals(cls)) return info;
    return null;
  }
  public String[] getPackagesForUid(int uid) {
    return uid == android.os.Process.myUid() ? new String[] {model.info.packageName} : null;
  }
  // Installing other packages is the launcher's job, never the game's.
  public boolean canRequestPackageInstalls() {
    return false;
  }
  public Intent getLaunchIntentForPackage(String pkg) {
    return model.info.packageName.equals(pkg) ? model.launchIntent() : null;
  }
  public static final int MATCH_DEFAULT_ONLY = 0x10000;
  public java.util.List<ResolveInfo> queryIntentActivities(Intent intent, int flags) {
    return new java.util.ArrayList<>(model.resolveActivities(intent, flags));
  }
  /** Installed packages visible to the app: with package visibility (API 30), its own. */
  public java.util.List<PackageInfo> getInstalledPackages(int flags) {
    try {
      return new java.util.ArrayList<>(java.util.List.of(getPackageInfo(model.info.packageName, flags)));
    } catch (NameNotFoundException e) {
      throw new IllegalStateException(e);
    }
  }
  /** Services are matched by explicit component; this framework records no service intent filters. */
  public ResolveInfo resolveService(Intent intent, int flags) {
    var matches = model.resolveServices(intent, flags);
    return matches.isEmpty() ? null : matches.get(0);
  }
  public ResolveInfo resolveActivity(Intent intent, int flags) {
    var matches = queryIntentActivities(intent, flags);
    return matches.isEmpty() ? null : matches.get(0);
  }
  public boolean hasSystemFeature(String name) {
    return hasSystemFeature(name, 0);
  }
  // XR supplies these features; optional Quest sensors are not exposed by Java.
  private static final java.util.Set<String> FEATURES =
      java.util.Set.of("android.hardware.vr.headtracking", "android.hardware.vr.high_performance",
          "android.software.vr.mode", FEATURE_BLUETOOTH, FEATURE_BLUETOOTH_LE);
  public FeatureInfo[] getSystemAvailableFeatures() {
    var features = new java.util.ArrayList<FeatureInfo>();
    FEATURES.stream()
        .sorted()
        .map(name -> {
          FeatureInfo feature = new FeatureInfo();
          feature.name = name;
          feature.version = 1;
          return feature;
        })
        .forEach(features::add);
    int version = RuntimeEnvironment.host().vulkanHardwareVersion();
    if (version != 0) {
      FeatureInfo level = new FeatureInfo();
      level.name = FEATURE_VULKAN_HARDWARE_LEVEL;
      level.version = 0;
      features.add(level);
      FeatureInfo api = new FeatureInfo();
      api.name = FEATURE_VULKAN_HARDWARE_VERSION;
      api.version = version;
      features.add(api);
    }
    return features.toArray(FeatureInfo[]::new);
  }
  public boolean hasSystemFeature(String name, int version) {
    if (FEATURES.contains(name)) return version <= 1;
    if (FEATURE_VULKAN_HARDWARE_LEVEL.equals(name))
      return version <= 0 && RuntimeEnvironment.host().vulkanHardwareVersion() != 0;
    if (FEATURE_VULKAN_HARDWARE_VERSION.equals(name)) {
      int available = RuntimeEnvironment.host().vulkanHardwareVersion();
      return available != 0 && version <= available;
    }
    return false;
  }
  public int checkPermission(String permission, String pkg) {
    return model.info.packageName.equals(pkg) && model.permissions.contains(permission)
            && java.util.Set
                   .of("android.permission.INTERNET", "android.permission.ACCESS_NETWORK_STATE",
                       "android.permission.BLUETOOTH", "android.permission.BLUETOOTH_ADMIN",
                       "android.permission.WAKE_LOCK", "android.permission.READ_EXTERNAL_STORAGE",
                       "android.permission.WRITE_EXTERNAL_STORAGE",
                       // Normal (install-time) permissions: granted when declared.
                       "android.permission.ACCESS_WIFI_STATE", "android.permission.CHANGE_WIFI_MULTICAST_STATE",
                       // Meta documents BODY_TRACKING as an automatically granted
                       // install-time permission. This grant does not imply XR support.
                       "com.oculus.permission.BODY_TRACKING")
                   .contains(permission)
        ? 0
        : -1;
  }
  // Android 12L's runtime ("dangerous") platform permissions; other platform permissions
  // are install-time. Unity reads each requested permission's level to pick what to request.
  private static final java.util.Set<String> DANGEROUS = java.util.Set.of(
      "READ_CALENDAR", "WRITE_CALENDAR", "CAMERA", "READ_CONTACTS", "WRITE_CONTACTS", "GET_ACCOUNTS",
      "ACCESS_FINE_LOCATION", "ACCESS_COARSE_LOCATION", "ACCESS_BACKGROUND_LOCATION", "RECORD_AUDIO",
      "READ_PHONE_STATE", "READ_PHONE_NUMBERS", "CALL_PHONE", "ANSWER_PHONE_CALLS", "READ_CALL_LOG",
      "WRITE_CALL_LOG", "ADD_VOICEMAIL", "USE_SIP", "PROCESS_OUTGOING_CALLS", "ACCEPT_HANDOVER", "BODY_SENSORS",
      "ACTIVITY_RECOGNITION", "SEND_SMS", "RECEIVE_SMS", "READ_SMS", "RECEIVE_WAP_PUSH", "RECEIVE_MMS",
      "READ_EXTERNAL_STORAGE", "WRITE_EXTERNAL_STORAGE", "ACCESS_MEDIA_LOCATION", "BLUETOOTH_SCAN",
      "BLUETOOTH_CONNECT", "BLUETOOTH_ADVERTISE", "UWB_RANGING");
  public PermissionInfo getPermissionInfo(String permission, int flags) throws NameNotFoundException {
    final String platform = "android.permission.";
    if (permission == null || !permission.startsWith(platform) || permission.length() == platform.length())
      throw new NameNotFoundException(permission);
    var info = new PermissionInfo();
    info.name = permission;
    info.packageName = "android";
    info.protectionLevel = DANGEROUS.contains(permission.substring(platform.length()))
        ? PermissionInfo.PROTECTION_DANGEROUS : PermissionInfo.PROTECTION_NORMAL;
    return info;
  }
  public CharSequence getApplicationLabel(ApplicationInfo info) {
    return info.nonLocalizedLabel != null ? info.nonLocalizedLabel : info.packageName;
  }
  public android.content.res.Resources getResourcesForApplication(ApplicationInfo info) {
    return RuntimeEnvironment.resources();
  }
  public String getInstallerPackageName(String pkg) {
    return null;
  }
}
