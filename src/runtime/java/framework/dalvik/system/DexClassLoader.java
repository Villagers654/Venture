package dalvik.system;
// API 26+ ignores optimizedDirectory; classes load as BaseDexClassLoader's do.
public class DexClassLoader extends BaseDexClassLoader {
  public DexClassLoader(String dexPath, String optimizedDirectory, String librarySearchPath, ClassLoader parent) {
    super(dexPath, null, librarySearchPath, parent);
  }
}
