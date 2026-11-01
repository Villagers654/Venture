package android.opengl;

/** GLES 2.0 on the calling thread's current context (ANGLE), as android.opengl.GLES20.
 *  Unreal's MediaPlayer14 bitmap renderer and Unity's video helpers draw with it on a
 *  context shared with the game's. Arrays and buffers are checked like Android's. */
public final class GLES20 {
  public static final int GL_VENDOR = 0x1f00, GL_RENDERER = 0x1f01,
      GL_VERSION = 0x1f02, GL_EXTENSIONS = 0x1f03;
  private GLES20() {}

  private static void array(Object array, int offset, int count, String name) {
    if (array == null) throw new IllegalArgumentException(name + " == null");
    if (offset < 0) throw new IllegalArgumentException(name + "Offset < 0");
    int length = java.lang.reflect.Array.getLength(array);
    if (length - offset < count) throw new IllegalArgumentException("length - " + name + "Offset < needed");
  }
  private static int elementSize(java.nio.Buffer b) {
    if (b instanceof java.nio.ByteBuffer) return 1;
    if (b instanceof java.nio.ShortBuffer || b instanceof java.nio.CharBuffer) return 2;
    if (b instanceof java.nio.LongBuffer || b instanceof java.nio.DoubleBuffer) return 8;
    return 4;
  }
  // A direct buffer, or a heap buffer's backing array; the native side pins arrays only for the call.
  private static Object base(java.nio.Buffer b) {
    if (b == null || b.isDirect()) return b;
    if (!b.hasArray()) throw new IllegalArgumentException("Buffer has no accessible storage");
    return b.array();
  }
  private static int offset(java.nio.Buffer b) {
    if (b == null) return 0;
    int size = elementSize(b);
    return ((b.isDirect() ? 0 : b.arrayOffset()) + b.position()) * size;
  }
  private static void remaining(java.nio.Buffer b, long bytes, String name) {
    if (b != null && (long) b.remaining() * elementSize(b) < bytes)
      throw new IllegalArgumentException("remaining() < needed (" + name + ")");
  }

  public static native String glGetString(int name);
  public static native int glGetError();
  public static native void glActiveTexture(int texture);
  public static native void glAttachShader(int program, int shader);
  public static native void glBindAttribLocation(int program, int index, String name);
  public static native void glBindBuffer(int target, int buffer);
  public static native void glBindFramebuffer(int target, int framebuffer);
  public static native void glBindRenderbuffer(int target, int renderbuffer);
  public static native void glBindTexture(int target, int texture);
  public static native void glBlendFunc(int sfactor, int dfactor);
  public static native int glCheckFramebufferStatus(int target);
  public static native void glClear(int mask);
  public static native void glClearColor(float red, float green, float blue, float alpha);
  public static native void glColorMask(boolean red, boolean green, boolean blue, boolean alpha);
  public static native void glCompileShader(int shader);
  public static native int glCreateProgram();
  public static native int glCreateShader(int type);
  public static native void glCullFace(int mode);
  public static native void glDeleteProgram(int program);
  public static native void glDeleteShader(int shader);
  public static native void glDepthMask(boolean flag);
  public static native void glDetachShader(int program, int shader);
  public static native void glDisable(int cap);
  public static native void glDisableVertexAttribArray(int index);
  public static native void glDrawArrays(int mode, int first, int count);
  public static native void glDrawElements(int mode, int count, int type, int offset);
  public static native void glEnable(int cap);
  public static native void glEnableVertexAttribArray(int index);
  public static native void glFinish();
  public static native void glFlush();
  public static native void glFramebufferRenderbuffer(int target, int attachment, int renderbuffertarget, int renderbuffer);
  public static native void glFramebufferTexture2D(int target, int attachment, int textarget, int texture, int level);
  public static native int glGetAttribLocation(int program, String name);
  public static native String glGetProgramInfoLog(int program);
  public static native String glGetShaderInfoLog(int shader);
  public static native int glGetUniformLocation(int program, String name);
  public static native boolean glIsEnabled(int cap);
  public static native void glLinkProgram(int program);
  public static native void glPixelStorei(int pname, int param);
  public static native void glRenderbufferStorage(int target, int internalformat, int width, int height);
  public static native void glScissor(int x, int y, int width, int height);
  public static native void glTexParameterf(int target, int pname, float param);
  public static native void glTexParameteri(int target, int pname, int param);
  public static native void glUniform1f(int location, float x);
  public static native void glUniform1i(int location, int x);
  public static native void glUniform2f(int location, float x, float y);
  public static native void glUniform4f(int location, float x, float y, float z, float w);
  public static native void glUseProgram(int program);
  public static native void glVertexAttribPointer(int index, int size, int type, boolean normalized, int stride, int offset);
  public static native void glViewport(int x, int y, int width, int height);

  private static native void names(int op, int n, int[] names, int offset);
  private static native void integers(int op, int a, int b, int[] params, int offset);
  private static native void floats(int op, int location, int count, boolean transpose, float[] value, int offset);
  private static native void memory(int op, int a, int b, int c, int d, int e, int f, int g, int h, Object base, int offset);
  private static native void attribPointer(int index, int size, int type, boolean normalized, int stride, java.nio.Buffer pointer, int offset);
  public static void glShaderSource(int shader, String string) {
    if (string == null) throw new IllegalArgumentException("string == null");
    shaderSource(shader, string);
  }
  private static native void shaderSource(int shader, String string);

  // names(): 0 textures, 1 buffers, 2 framebuffers, 3 renderbuffers; generate +0, delete +4.
  private static void names(int op, int n, int[] names, int offset, String what) {
    if (n < 0) throw new IllegalArgumentException("n < 0");
    array(names, offset, n, what);
    names(op, n, names, offset);
  }
  public static void glGenTextures(int n, int[] textures, int offset) { names(0, n, textures, offset, "textures"); }
  public static void glGenBuffers(int n, int[] buffers, int offset) { names(1, n, buffers, offset, "buffers"); }
  public static void glGenFramebuffers(int n, int[] framebuffers, int offset) { names(2, n, framebuffers, offset, "framebuffers"); }
  public static void glGenRenderbuffers(int n, int[] renderbuffers, int offset) { names(3, n, renderbuffers, offset, "renderbuffers"); }
  public static void glDeleteTextures(int n, int[] textures, int offset) { names(4, n, textures, offset, "textures"); }
  public static void glDeleteBuffers(int n, int[] buffers, int offset) { names(5, n, buffers, offset, "buffers"); }
  public static void glDeleteFramebuffers(int n, int[] framebuffers, int offset) { names(6, n, framebuffers, offset, "framebuffers"); }
  public static void glDeleteRenderbuffers(int n, int[] renderbuffers, int offset) { names(7, n, renderbuffers, offset, "renderbuffers"); }

  // integers(): 0 glGetIntegerv, 1 glGetShaderiv, 2 glGetProgramiv, 3 glGetTexParameteriv.
  // Every query writes at most 4 values except GL_COMPRESSED_TEXTURE_FORMATS and friends.
  private static void query(int op, int a, int b, int[] params, int offset) {
    array(params, offset, 1, "params");
    integers(op, a, b, params, offset);
  }
  public static void glGetIntegerv(int pname, int[] params, int offset) { query(0, pname, 0, params, offset); }
  public static void glGetShaderiv(int shader, int pname, int[] params, int offset) { query(1, shader, pname, params, offset); }
  public static void glGetProgramiv(int program, int pname, int[] params, int offset) { query(2, program, pname, params, offset); }
  public static void glGetTexParameteriv(int target, int pname, int[] params, int offset) { query(3, target, pname, params, offset); }

  // floats(): 0 glUniformMatrix4fv, 1 glUniform4fv, 2 glUniform1fv.
  public static void glUniformMatrix4fv(int location, int count, boolean transpose, float[] value, int offset) {
    if (count < 0) throw new IllegalArgumentException("count < 0");
    array(value, offset, count * 16, "value");
    floats(0, location, count, transpose, value, offset);
  }
  public static void glUniform4fv(int location, int count, float[] v, int offset) {
    if (count < 0) throw new IllegalArgumentException("count < 0");
    array(v, offset, count * 4, "v");
    floats(1, location, count, false, v, offset);
  }
  public static void glUniform1fv(int location, int count, float[] v, int offset) {
    if (count < 0) throw new IllegalArgumentException("count < 0");
    array(v, offset, count, "v");
    floats(2, location, count, false, v, offset);
  }

  // memory(): 0 glTexImage2D, 1 glTexSubImage2D, 2 glReadPixels, 3 glBufferData, 4 glBufferSubData.
  public static void glTexImage2D(int target, int level, int internalformat, int width, int height, int border,
      int format, int type, java.nio.Buffer pixels) {
    memory(0, target, level, internalformat, width, height, border, format, type, base(pixels), offset(pixels));
  }
  public static void glTexSubImage2D(int target, int level, int xoffset, int yoffset, int width, int height,
      int format, int type, java.nio.Buffer pixels) {
    if (pixels == null) throw new IllegalArgumentException("pixels == null");
    memory(1, target, level, xoffset, yoffset, width, height, format, type, base(pixels), offset(pixels));
  }
  public static void glReadPixels(int x, int y, int width, int height, int format, int type, java.nio.Buffer pixels) {
    if (pixels == null) throw new IllegalArgumentException("pixels == null");
    memory(2, x, y, width, height, format, type, 0, 0, base(pixels), offset(pixels));
  }
  public static void glBufferData(int target, int size, java.nio.Buffer data, int usage) {
    if (size < 0) throw new IllegalArgumentException("size < 0");
    remaining(data, size, "data");
    memory(3, target, size, usage, 0, 0, 0, 0, 0, base(data), offset(data));
  }
  public static void glBufferSubData(int target, int offset, int size, java.nio.Buffer data) {
    if (data == null) throw new IllegalArgumentException("data == null");
    remaining(data, size, "data");
    memory(4, target, offset, size, 0, 0, 0, 0, 0, base(data), offset(data));
  }
  public static void glVertexAttribPointer(int index, int size, int type, boolean normalized, int stride,
      java.nio.Buffer pointer) {
    // GL keeps the client pointer until the draw: Android requires direct storage.
    if (pointer == null || !pointer.isDirect()) throw new IllegalArgumentException("Must use a native order direct Buffer");
    attribPointer(index, size, type, normalized, stride, pointer, offset(pointer));
  }
}
