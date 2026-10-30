package android.content;
public final class ComponentName implements android.os.Parcelable {
  private final String pkg, cls;
  public ComponentName(String p, String c) {
    pkg = p;
    cls = c.startsWith(".") ? p + c : c;
  }
  public ComponentName(Context c, String cls) {
    this(c.getPackageName(), cls);
  }
  public ComponentName(Context c, Class<?> t) {
    this(c.getPackageName(), t.getName());
  }
  public String getPackageName() {
    return pkg;
  }
  public String getClassName() {
    return cls;
  }
  public String flattenToString() {
    return pkg + "/" + cls;
  }
  public String flattenToShortString() {
    return pkg + "/" + (cls.startsWith(pkg + ".") ? cls.substring(pkg.length()) : cls);
  }
  public String toString() {
    return flattenToString();
  }
  public boolean equals(Object o) {
    return o instanceof ComponentName c && pkg.equals(c.pkg) && cls.equals(c.cls);
  }
  public int hashCode() {
    return pkg.hashCode() * 31 + cls.hashCode();
  }
  public int describeContents() {
    return 0;
  }
  public void writeToParcel(android.os.Parcel p, int f) {
    p.writeString(pkg);
    p.writeString(cls);
  }
}
