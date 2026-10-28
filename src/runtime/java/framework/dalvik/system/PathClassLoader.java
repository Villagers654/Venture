package dalvik.system;
public class PathClassLoader extends BaseDexClassLoader {
  public PathClassLoader(String path, ClassLoader parent) {
    this(path, null, parent);
  }
  public PathClassLoader(String path, String libs, ClassLoader parent) {
    super(path, null, libs, parent);
  }
  public Class<?> loadClass(String name, boolean resolve) throws ClassNotFoundException {
    return super.loadClass(name, resolve);
  }
}
