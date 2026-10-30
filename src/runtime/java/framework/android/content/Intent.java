package android.content;
import android.net.Uri;
import android.os.*;
import java.util.*;
public class Intent implements Parcelable {
  public static final String ACTION_MAIN = "android.intent.action.MAIN",
                             ACTION_VIEW = "android.intent.action.VIEW",
                             CATEGORY_LAUNCHER = "android.intent.category.LAUNCHER",
                             ACTION_BATTERY_CHANGED = "android.intent.action.BATTERY_CHANGED",
                             ACTION_HEADSET_PLUG = "android.intent.action.HEADSET_PLUG";
  public static final int FLAG_ACTIVITY_NEW_TASK = 0x10000000;
  public static final int FLAG_ACTIVITY_CLEAR_TASK = 0x00008000;
  public static final int FLAG_ACTIVITY_CLEAR_TOP = 0x04000000;
  public static final int URI_INTENT_SCHEME = 1, URI_ANDROID_APP_SCHEME = 2,
                          URI_ALLOW_UNSAFE = 4;
  private String action, pkg, type;
  private ComponentName component;
  private Uri data;
  private int flags;
  private final Bundle extras = new Bundle();
  private final Set<String> categories = new HashSet<>();
  public static final String ACTION_CHOOSER = "android.intent.action.CHOOSER",
      EXTRA_INTENT = "android.intent.extra.INTENT", EXTRA_TITLE = "android.intent.extra.TITLE";
  public Intent() {}
  public static Intent createChooser(Intent target, CharSequence title) {
    var chooser = new Intent(ACTION_CHOOSER);
    chooser.extras.putParcelable(EXTRA_INTENT, target);
    if (title != null) chooser.extras.putCharSequence(EXTRA_TITLE, title);
    return chooser;
  }
  public Intent(String a) {
    action = a;
  }
  public Intent(String a, Uri u) {
    action = a;
    data = u;
  }
  public Intent(Context c, Class<?> cls) {
    component = new ComponentName(c, cls);
  }
  public Intent(Intent i) {
    action = i.action;
    pkg = i.pkg;
    type = i.type;
    component = i.component;
    data = i.data;
    flags = i.flags;
    extras.putAll(i.extras);
    categories.addAll(i.categories);
  }
  public String getAction() {
    return action;
  }
  public Intent setAction(String a) {
    action = a;
    return this;
  }
  public String getPackage() {
    return pkg;
  }
  public Intent setPackage(String p) {
    pkg = p;
    return this;
  }
  public ComponentName getComponent() {
    return component;
  }
  public Intent setComponent(ComponentName c) {
    component = c;
    return this;
  }
  public Intent setClassName(String p, String c) {
    return setComponent(new ComponentName(p, c));
  }
  public Intent setClass(Context c, Class<?> t) {
    return setComponent(new ComponentName(c, t));
  }
  public Intent setData(Uri u) {
    data = u;
    type = null;
    return this;
  }
  public Uri getData() {
    return data;
  }
  public String getDataString() {
    return data == null ? null : data.toString();
  }
  public Intent setType(String t) {
    type = t;
    data = null;
    return this;
  }
  public String getType() {
    return type;
  }
  public Intent setDataAndType(Uri uri, String mimeType) { data = uri; type = mimeType; return this; }
  public String getScheme() { return data == null ? null : data.getScheme(); }
  public String resolveType(ContentResolver resolver) {
    return type != null ? type : data != null && "content".equals(data.getScheme()) ? resolver.getType(data) : null;
  }
  public ComponentName resolveActivity(android.content.pm.PackageManager manager) {
    if (component != null) return component;
    var resolved = manager.resolveActivity(this, android.content.pm.PackageManager.MATCH_DEFAULT_ONLY);
    return resolved == null ? null : new ComponentName(resolved.activityInfo.packageName, resolved.activityInfo.name);
  }
  public Intent addCategory(String c) {
    categories.add(c);
    return this;
  }
  public Set<String> getCategories() {
    return Set.copyOf(categories);
  }
  public Intent setFlags(int f) {
    flags = f;
    return this;
  }
  public Intent addFlags(int f) {
    flags |= f;
    return this;
  }
  public int getFlags() {
    return flags;
  }
  public Intent putExtra(String k, String v) {
    extras.putString(k, v);
    return this;
  }
  public Intent putExtra(String k, int v) {
    extras.putInt(k, v);
    return this;
  }
  public Intent putExtra(String k, boolean v) {
    extras.putBoolean(k, v);
    return this;
  }
  public Intent putExtra(String k, long v) {
    extras.putLong(k, v);
    return this;
  }
  public Intent putExtra(String k, Parcelable v) {
    extras.putParcelable(k, v);
    return this;
  }
  public Intent putExtra(String k, String[] v) { extras.putStringArray(k, v); return this; }
  public Intent putExtra(String k, int[] v) { extras.putIntArray(k, v); return this; }
  public Intent putExtra(String k, byte[] v) { extras.putByteArray(k, v); return this; }
  public String[] getStringArrayExtra(String k) { return extras.getStringArray(k); }
  public int[] getIntArrayExtra(String k) { return extras.getIntArray(k); }
  public byte[] getByteArrayExtra(String k) { return extras.getByteArray(k); }
  public Intent putExtra(String k, Bundle v) { extras.putBundle(k, v); return this; }
  public Intent putExtra(String k, java.io.Serializable v) { extras.putSerializable(k, v); return this; }
  public Intent putExtra(String k, float v) { extras.putFloat(k, v); return this; }
  public Intent putExtra(String k, double v) { extras.putDouble(k, v); return this; }
  public Intent putExtra(String k, CharSequence v) { extras.putCharSequence(k, v); return this; }
  public Bundle getBundleExtra(String k) { return extras.getBundle(k); }
  public java.io.Serializable getSerializableExtra(String k) { return extras.getSerializable(k); }
  public float getFloatExtra(String k, float d) { return extras.getFloat(k, d); }
  public double getDoubleExtra(String k, double d) { return extras.getDouble(k, d); }
  public CharSequence getCharSequenceExtra(String k) { return extras.getCharSequence(k); }
  public void setExtrasClassLoader(ClassLoader loader) {}
  public static Intent makeMainActivity(ComponentName mainActivity) {
    return new Intent(ACTION_MAIN).setComponent(mainActivity).addCategory(CATEGORY_LAUNCHER);
  }
  public Intent putExtras(Bundle b) {
    extras.putAll(b);
    return this;
  }
  public Bundle getExtras() {
    return new Bundle(extras);
  }
  public boolean hasExtra(String k) {
    return extras.containsKey(k);
  }
  public String getStringExtra(String k) {
    return extras.getString(k);
  }
  public int getIntExtra(String k, int d) {
    return extras.getInt(k, d);
  }
  public long getLongExtra(String k, long d) {
    return extras.getLong(k, d);
  }
  public boolean getBooleanExtra(String k, boolean d) {
    return extras.getBoolean(k, d);
  }
  public <T extends Parcelable> T getParcelableExtra(String k) {
    return extras.getParcelable(k);
  }
  public void removeExtra(String k) {
    extras.remove(k);
  }
  // Android 12L core/java/android/content/Intent.java, toUri/toUriInner.
  // Launch details use this serialization through Unity's AndroidJavaObject.
  public String toUri(int uriFlags) {
    StringBuilder base = new StringBuilder();
    String scheme = null, defaultAction = ACTION_VIEW, defaultPackage = null;
    String text = data == null ? "" : data.toString();
    if ((uriFlags & URI_ANDROID_APP_SCHEME) != 0) {
      if (pkg == null) throw new IllegalArgumentException("android-app URI requires a package");
      base.append("android-app://").append(pkg);
      defaultPackage = pkg;
      scheme = data == null ? null : data.getScheme();
      defaultAction = scheme == null ? ACTION_MAIN : ACTION_VIEW;
      if (scheme != null) {
        base.append('/').append(scheme);
        if (data.getEncodedAuthority() != null) {
          base.append('/').append(data.getEncodedAuthority());
          if (data.getEncodedPath() != null) base.append(data.getEncodedPath());
          if (data.getEncodedQuery() != null) base.append('?').append(data.getEncodedQuery());
          if (data.getEncodedFragment() != null) base.append('#').append(data.getEncodedFragment());
        }
      }
      scheme = null; // The android-app base already encodes the data scheme.
    } else {
      if ((uriFlags & URI_INTENT_SCHEME) != 0) {
        int colon = text.indexOf(':');
        if (colon > 0 && text.substring(0, colon).matches("[a-zA-Z0-9.+-]+")) {
          scheme = text.substring(0, colon);
          text = text.substring(colon + 1);
          base.append("intent:");
        } else if (data == null) base.append("intent:");
      }
      base.append(text);
    }
    StringBuilder fragment = new StringBuilder();
    if (scheme != null) fragment.append("scheme=").append(scheme).append(';');
    if (action != null && !action.equals(defaultAction)) uriField(fragment, "action", action, null);
    for (String category : categories) uriField(fragment, "category", category, null);
    if (type != null) uriField(fragment, "type", type, "/");
    if (flags != 0) fragment.append("launchFlags=0x").append(Integer.toHexString(flags)).append(';');
    if (pkg != null && !pkg.equals(defaultPackage)) uriField(fragment, "package", pkg, null);
    if (component != null) uriField(fragment, "component", component.flattenToShortString(), "/");
    for (String key : extras.keySet()) {
      Object v = extras.get(key);
      char kind = v instanceof String ? 'S' : v instanceof Boolean ? 'B' :
          v instanceof Byte ? 'b' : v instanceof Character ? 'c' :
          v instanceof Double ? 'd' : v instanceof Float ? 'f' :
          v instanceof Integer ? 'i' : v instanceof Long ? 'l' : v instanceof Short ? 's' : 0;
      // Android's URI format deliberately omits arrays and Parcelable extras.
      if (kind != 0) uriField(fragment, kind + "." + Uri.encode(key), v.toString(), null);
    }
    if (!fragment.isEmpty()) base.append("#Intent;").append(fragment).append("end");
    return base.toString();
  }
  private static void uriField(StringBuilder out, String key, String value, String allow) {
    out.append(key).append('=').append(Uri.encode(value, allow)).append(';');
  }
  public int describeContents() {
    return 0;
  }
  public void writeToParcel(Parcel p, int f) {
    p.writeString(action);
    p.writeBundle(extras);
  }
}
