package android.content.pm;

public class ConfigurationInfo {
  public static final int GL_ES_VERSION_UNDEFINED = 0;
  // Quest 3 offers OpenGL ES 3.2.
  public int reqGlEsVersion = 0x30002, reqTouchScreen = 1, reqKeyboardType = 1, reqNavigation = 1, reqInputFeatures;
  public String getGlEsVersion() {
    return (reqGlEsVersion >> 16) + "." + (reqGlEsVersion & 0xffff);
  }
}
