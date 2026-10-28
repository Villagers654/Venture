package android.opengl;
public final class GLES20 {
  public static final int GL_VENDOR = 0x1f00, GL_RENDERER = 0x1f01,
      GL_VERSION = 0x1f02, GL_EXTENSIONS = 0x1f03;
  public static native String glGetString(int name);
  public static native int glGetError();
}
