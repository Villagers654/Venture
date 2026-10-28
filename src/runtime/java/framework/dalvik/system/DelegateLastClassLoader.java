package dalvik.system;
public class DelegateLastClassLoader extends PathClassLoader {
  public DelegateLastClassLoader(String p, ClassLoader parent) {
    super(p, parent);
  }
  public DelegateLastClassLoader(String p, String libs, ClassLoader parent) {
    super(p, libs, parent);
  }
  public Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
    name = binaryName(name);
    if (name.startsWith("java.") || name.startsWith("javax.")
        || name.startsWith("android.") && !name.startsWith("android.support.")
        || name.startsWith("com.venture.") || name.startsWith("dalvik.")
        || name.startsWith("org.json.") || name.startsWith("org.xmlpull."))
      return super.loadClass(name, resolve);
    synchronized (getClassLoadingLock(name)) {
      Class<?> c = findLoadedClass(name);
      if (c == null)
        try {
          c = findClass(name);
        } catch (ClassNotFoundException e) {
          c = super.loadClass(name, false);
        }
      if (resolve)
        resolveClass(c);
      return c;
    }
  }
}
