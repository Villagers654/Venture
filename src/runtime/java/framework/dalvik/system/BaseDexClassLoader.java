package dalvik.system;
public class BaseDexClassLoader extends java.net.URLClassLoader {
  private final String libraries;
  public BaseDexClassLoader(String path, java.io.File optimized, String libs, ClassLoader parent) {
    super(urls(path), parent);
    libraries = libs;
  }
  private static java.net.URL[] urls(String path) {
    return java.util.Arrays
        .stream(path.split(java.util.regex.Pattern.quote(java.io.File.pathSeparator)))
        .filter(s -> !s.isEmpty())
        .map(s -> {
          if (!s.endsWith(".jar"))
            throw new UnsupportedOperationException("DEX must be converted before loading: " + s);
          try {
            return new java.io.File(s).toURI().toURL();
          } catch (java.net.MalformedURLException e) {
            throw new IllegalArgumentException(e);
          }
        })
        .toArray(java.net.URL[] ::new);
  }
  // Optional SDK contracts exist only to link callback signatures retained by
  // Unity. Prefer the APK's own SDK implementation whenever it ships one.
  @Override
  protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
    name = binaryName(name);
    if (!name.startsWith("com.google.android.gms.tasks.")
        && !name.startsWith("com.google.android.play.core.tasks.")
        && !name.startsWith("com.google.android.play.core.assetpacks.")
        && !name.startsWith("com.google.android.play.core.listener."))
      return super.loadClass(name, resolve);
    synchronized (getClassLoadingLock(name)) {
      Class<?> cls = findLoadedClass(name);
      if (cls == null) {
        try {
          cls = findClass(name);
        } catch (ClassNotFoundException absent) {
          cls = super.loadClass(name, false);
        }
      }
      if (resolve)
        resolveClass(cls);
      return cls;
    }
  }
  @Override
  public java.net.URL getResource(String name) {
    if (name.startsWith("com/google/android/gms/tasks/")
        || name.startsWith("com/google/android/play/core/tasks/")
        || name.startsWith("com/google/android/play/core/assetpacks/")
        || name.startsWith("com/google/android/play/core/listener/")) {
      java.net.URL own = findResource(name);
      if (own != null)
        return own;
    }
    return super.getResource(name);
  }
  protected Class<?> findClass(String name) throws ClassNotFoundException {
    name = binaryName(name);
    synchronized (getClassLoadingLock(name)) {
      // ART's DefineClass returns an existing definition for this loader. UE4
      // calls findClass directly through JNI, bypassing loadClass's cache check.
      Class<?> cls = findLoadedClass(name);
      if (cls == null) {
        cls = super.findClass(name);
        com.venture.runtime.RuntimeEnvironment.host().bindClass(cls);
      }
      return cls;
    }
  }
  protected static String binaryName(String name) {
    // ART dalvik_system_DexFile.cc uses DotToDescriptor (descriptors_names.cc),
    // which accepts both dotted names and names already containing JNI slashes.
    return name.replace('/', '.');
  }
  public String findLibrary(String name) {
    if (name.contains("/") || name.contains("\\"))
      throw new IllegalArgumentException("Not a library name");
    String file = "lib" + name + ".so";
    var env = com.venture.runtime.RuntimeEnvironment.get();
    return new java.io.File(env.nativeLib, file).isFile()
        ? env.guestNativeLibDir() + "/" + file
        : null;
  }
}
