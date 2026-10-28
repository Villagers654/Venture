package android.content.pm;
public class FeatureInfo {
  public static final int FLAG_REQUIRED = 1, GL_ES_VERSION_UNDEFINED = 0;
  public String name;
  public int version, reqGlEsVersion, flags;
  public FeatureInfo() {}
  public FeatureInfo(FeatureInfo other) { name=other.name; version=other.version; reqGlEsVersion=other.reqGlEsVersion; flags=other.flags; }
  public String getGlEsVersion() { return ((reqGlEsVersion >> 16) & 0xffff) + "." + (reqGlEsVersion & 0xffff); }
  @Override public String toString() {
    // AOSP 12L FeatureInfo: UE4 parses the version from this diagnostic string.
    return "FeatureInfo{" + Integer.toHexString(System.identityHashCode(this)) + " "
        + (name != null ? name + " v=" + version : "glEsVers=" + getGlEsVersion())
        + " fl=0x" + Integer.toHexString(flags) + "}";
  }
}
