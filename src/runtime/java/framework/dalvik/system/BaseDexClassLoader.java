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
  // Optional SDK contracts exist only to link signatures retained by Unity
  // callbacks and by SDKs that probe for androidx.lifecycle. Prefer the APK's
  // own SDK implementation whenever it ships one.
  private static final String[] APK_FIRST = {
    "com.google.android.gms.tasks.", "com.google.android.play.core.tasks.",
    "com.google.android.play.core.assetpacks.", "com.google.android.play.core.listener.",
    "androidx.lifecycle.",
  };
  private static boolean apkFirst(String binaryName) {
    for (String prefix : APK_FIRST)
      if (binaryName.startsWith(prefix))
        return true;
    return false;
  }
  @Override
  protected Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
    name = binaryName(name);
    if (!apkFirst(name))
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
    if (apkFirst(name.replace('/', '.'))) {
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
  // ART's DexPathList element for an APK serves every zip entry as a Java resource
  // (META-INF/services for ServiceLoader, OkHttp's publicsuffixes.gz). The converted
  // class jar holds only classes, so resources come from the APK itself.
  private java.util.zip.ZipFile resourceApk;
  private String resourceBase;
  private java.net.URLStreamHandler resourceHandler;
  public final synchronized void ventureResourceApk(java.io.File apk) throws java.io.IOException {
    if (resourceApk != null) throw new IllegalStateException("APK resources already attached");
    var zip = new java.util.zip.ZipFile(apk);
    resourceBase = "jar:" + apk.toURI() + "!/";
    resourceHandler = new java.net.URLStreamHandler() {
      @Override protected java.net.URLConnection openConnection(java.net.URL url) {
        String entryName = url.toString().substring(resourceBase.length());
        return new java.net.URLConnection(url) {
          @Override public void connect() {}
          @Override public java.io.InputStream getInputStream() throws java.io.IOException {
            var entry = zip.getEntry(entryName);
            if (entry == null) throw new java.io.FileNotFoundException(entryName);
            return zip.getInputStream(entry);
          }
          @Override public long getContentLengthLong() {
            var entry = zip.getEntry(entryName);
            return entry == null ? -1 : entry.getSize();
          }
        };
      }
    };
    resourceApk = zip;
  }
  private synchronized java.net.URL apkResource(String name) {
    if (resourceApk == null || name.isEmpty() || name.startsWith("/")) return null;
    var entry = resourceApk.getEntry(name);
    if (entry == null || entry.isDirectory()) return null;
    try {
      return new java.net.URL(null, resourceBase + name, resourceHandler);
    } catch (java.net.MalformedURLException invalid) {
      return null;
    }
  }
  @Override
  public java.net.URL findResource(String name) {
    java.net.URL own = super.findResource(name);
    return own != null ? own : apkResource(name);
  }
  @Override
  public java.util.Enumeration<java.net.URL> findResources(String name) throws java.io.IOException {
    var found = java.util.Collections.list(super.findResources(name));
    java.net.URL apk = apkResource(name);
    if (apk != null) found.add(apk);
    return java.util.Collections.enumeration(found);
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
