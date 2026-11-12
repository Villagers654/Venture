package android.opengl;

import android.graphics.Bitmap;

// AOSP opengl/java/android/opengl/GLUtils.java: uploads a Bitmap's pixels to the
// current GL context's bound texture. ARGB_8888 bitmaps upload as RGBA bytes,
// premultiplied as Android stores them.
public final class GLUtils {
  private static final int GL_RGBA = 0x1908, GL_UNSIGNED_BYTE = 0x1401;
  private GLUtils() {}
  private static java.nio.ByteBuffer pixels(Bitmap bitmap) {
    if (bitmap == null) throw new NullPointerException("texImage2D can't be used with a null Bitmap");
    if (bitmap.isRecycled()) throw new IllegalArgumentException("bitmap is recycled");
    var pixels = java.nio.ByteBuffer.allocateDirect(bitmap.getWidth() * bitmap.getHeight() * 4);
    bitmap.copyPixelsToBuffer(pixels);
    return pixels.flip();
  }
  public static int getInternalFormat(Bitmap bitmap) {
    if (bitmap.isRecycled()) throw new IllegalArgumentException("bitmap is recycled");
    return GL_RGBA;
  }
  public static int getType(Bitmap bitmap) {
    if (bitmap.isRecycled()) throw new IllegalArgumentException("bitmap is recycled");
    return GL_UNSIGNED_BYTE;
  }
  public static void texImage2D(int target, int level, int internalformat, Bitmap bitmap, int type, int border) {
    var data = pixels(bitmap);
    GLES20.glTexImage2D(target, level, internalformat < 0 ? GL_RGBA : internalformat, bitmap.getWidth(),
        bitmap.getHeight(), border, GL_RGBA, type < 0 ? GL_UNSIGNED_BYTE : type, data);
  }
  public static void texImage2D(int target, int level, int internalformat, Bitmap bitmap, int border) {
    texImage2D(target, level, internalformat, bitmap, -1, border);
  }
  public static void texImage2D(int target, int level, Bitmap bitmap, int border) {
    texImage2D(target, level, -1, bitmap, -1, border);
  }
  public static void texSubImage2D(int target, int level, int xoffset, int yoffset, Bitmap bitmap) {
    texSubImage2D(target, level, xoffset, yoffset, bitmap, GL_RGBA, GL_UNSIGNED_BYTE);
  }
  public static void texSubImage2D(int target, int level, int xoffset, int yoffset, Bitmap bitmap, int format,
      int type) {
    var data = pixels(bitmap);
    GLES20.glTexSubImage2D(target, level, xoffset, yoffset, bitmap.getWidth(), bitmap.getHeight(),
        format < 0 ? GL_RGBA : format, type < 0 ? GL_UNSIGNED_BYTE : type, data);
  }
}
