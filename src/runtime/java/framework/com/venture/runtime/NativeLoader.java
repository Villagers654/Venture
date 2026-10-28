package com.venture.runtime;

import java.lang.reflect.*;
import java.util.Objects;

/** Rewritten APK load sites enter the guest ELF linker with the original caller's loader. */
public final class NativeLoader {
  private NativeLoader() {}
  private static final StackWalker WALKER =
      StackWalker.getInstance(StackWalker.Option.RETAIN_CLASS_REFERENCE);
  private static ClassLoader callerLoader() {
    return WALKER.walk(s
        -> s.map(StackWalker.StackFrame::getDeclaringClass)
            .filter(c -> c != NativeLoader.class)
            .findFirst()
            .orElseThrow()
            .getClassLoader());
  }
  public static void loadLibrary(String name) {
    library(name, callerLoader());
  }
  private static void library(String name, ClassLoader loader) {
    Objects.requireNonNull(name);
    if (name.isEmpty() || name.indexOf('/') >= 0 || name.indexOf('\\') >= 0)
      throw new UnsatisfiedLinkError("Invalid guest library name: " + name);
    RuntimeEnvironment.host().loadLibrary("lib" + name + ".so", loader);
  }
  public static void load(String path) {
    path(path, callerLoader());
  }
  private static void path(String path, ClassLoader loader) {
    Objects.requireNonNull(path);
    if (!path.startsWith("/data/") && !path.startsWith("/sdcard/") && !path.startsWith("/system/")
        && !path.startsWith("/vendor/") && !path.startsWith("/storage/emulated/0/")) {
      if (!new java.io.File(path).isAbsolute())
        throw new UnsatisfiedLinkError("System.load requires an absolute path");
      try {
        path = RuntimeEnvironment.toGuestPath(path);
      } catch (IllegalArgumentException e) {
        throw new UnsatisfiedLinkError("System.load path is outside application mounts");
      }
    }
    RuntimeEnvironment.host().loadLibrary(path, loader);
  }
  // Method-handle adaptations preserve Runtime's receiver parameter and null behavior.
  public static void loadLibrary(Runtime runtime, String name) {
    Objects.requireNonNull(runtime);
    library(name, callerLoader());
  }
  public static void load(Runtime runtime, String path) {
    Objects.requireNonNull(runtime);
    path(path, callerLoader());
  }

  // Android retains these Runtime entry points for reflected native loads.
  // Keep the supplied initiating loader; HotSpot's private Runtime methods
  // load host libraries and are not suitable for guest ELF objects.
  public static void loadLibrary0(Runtime runtime, ClassLoader loader, String name) {
    Objects.requireNonNull(runtime);
    library(name, loader);
  }
  public static void loadLibrary0(Runtime runtime, Class<?> caller, String name) {
    loadLibrary0(runtime, Objects.requireNonNull(caller).getClassLoader(), name);
  }
  public static void loadLibrary0(Runtime runtime, ClassLoader loader, Class<?> caller, String name) {
    loadLibrary0(runtime, loader, name);
  }
  public static void load0(Runtime runtime, ClassLoader loader, String filename) {
    Objects.requireNonNull(runtime);
    path(filename, loader);
  }
  public static void load0(Runtime runtime, Class<?> caller, String filename) {
    load0(runtime, Objects.requireNonNull(caller).getClassLoader(), filename);
  }
  public static void load0(Runtime runtime, ClassLoader loader, Class<?> caller, String filename) {
    load0(runtime, loader, filename);
  }
  private static native void load0(String name, ClassLoader loader);
  private static native void exit0(int status);
  // Android's app process owns the guest workers, audio and JVM together.
  // HotSpot Runtime.exit would invoke host shutdown while those workers live.
  public static void exit(int status) {
    // Retain the worker-wide decision across intent claim and the host's
    // synchronous restart preparation. Concurrent Java or native exits cannot
    // terminate the worker before the supervisor receives the restart packet.
    var host = RuntimeEnvironment.host();
    host.beginProcessExit();
    try {
      android.util.Log.i("Process", "Application requested process exit " + status);
      WALKER.walk(s -> {
        s.filter(f -> f.getDeclaringClass() != NativeLoader.class).limit(8)
            .forEach(f -> android.util.Log.i("Process", "Exit caller: " + f));
        return null;
      });
      if (status == 0 && AppMain.exitStatus() == 0) {
        var payload = AppMain.takePendingRelaunchPayload();
        if (payload != null) {
          RuntimeEnvironment.host().restartProcess(payload);
          throw new ThreadDeath();
        }
      }
      AppMain.discardPendingRelaunch();
      com.venture.runtime.AppMain.requestShutdown();
      RuntimeEnvironment.host().exitProcess(status == 0 ? AppMain.exitStatus() : status);
      throw new ThreadDeath();
    } finally {
      host.endProcessExit();
    }
  }

  public static void exit(Runtime runtime, int status) {
    Objects.requireNonNull(runtime);
    exit(status);
  }
  /**
   * App classloaders call after defineClass, before initialization, to bind late Java_* exports.
   */
  public static native void bindClass(Class<?> cls);

  public static String descriptor(Executable method) {
    StringBuilder s = new StringBuilder("(");
    for (Class<?> p : method.getParameterTypes()) s.append(p.descriptorString());
    return s.append(')')
        .append(method instanceof Method m ? m.getReturnType().descriptorString() : "V")
        .toString();
  }
  public static int loadKind(Executable method) {
    Class<?> c = method.getDeclaringClass();
    if (c == Method.class && method.getName().equals("invoke"))
      return 3;
    if ((c == System.class || c == Runtime.class) && method.getName().equals("exit")
        && descriptor(method).equals("(I)V")) return 4;
    if ((c == System.class || c == Runtime.class)
        && descriptor(method).equals("(Ljava/lang/String;)V")) {
      if (method.getName().equals("loadLibrary"))
        return 1;
      if (method.getName().equals("load"))
        return 2;
    }
    return 0;
  }
  /** Retains reflection exceptions while intercepting reflected ELF loads. */
  public static Object invoke(Method method, Object target, Object[] args)
      throws IllegalAccessException, InvocationTargetException {
    return invokeWithLoader(method, target, args, callerLoader());
  }
  public static Object invokeWithLoader(Method method, Object target, Object[] args,
      ClassLoader loader) throws IllegalAccessException, InvocationTargetException {
    int kind = loadKind(method);
    if (kind == 0 || kind == 3)
      return method.invoke(target, args);
    boolean stat = Modifier.isStatic(method.getModifiers());
    if (!stat && target == null)
      throw new NullPointerException("Runtime.load receiver");
    if (!stat && !method.getDeclaringClass().isInstance(target))
      throw new IllegalArgumentException("wrong receiver");
    if (!method.canAccess(stat ? null : target))
      throw new IllegalAccessException("native-load method access");
    if (args == null || args.length != 1 || (kind == 4 ? !exitArgument(args[0])
        : (args[0] != null && !(args[0] instanceof String))))
      throw new IllegalArgumentException("native-load arguments");
    try {
      if (kind != 4 && MethodHooks.invokeLoadHook(method, stat ? method.getDeclaringClass() : target, (String) args[0]))
        return null;
      if (kind == 4) exit(args[0] instanceof Character c ? c : ((Number) args[0]).intValue());
      else if (kind == 1)
        library((String) args[0], loader);
      else
        path((String) args[0], loader);
      return null;
    } catch (Throwable e) {
      throw new InvocationTargetException(e);
    }
  }
  // Method.invoke performs unboxing followed by primitive widening to int.
  private static boolean exitArgument(Object value) {
    return value instanceof Byte || value instanceof Short || value instanceof Character || value instanceof Integer;
  }
  // Class-file metadata does not resolve types used only by optional Java methods.
  public static String[] nativeMethods(Class<?> cls) {
    return NativeMethods.declarations(cls);
  }
}
