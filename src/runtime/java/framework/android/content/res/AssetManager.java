package android.content.res;
import com.venture.runtime.RuntimeEnvironment;
import com.venture.runtime.AssetReadTrace;
import java.io.*;
public class AssetManager implements AutoCloseable {
  public static final int ACCESS_UNKNOWN = 0, ACCESS_RANDOM = 1, ACCESS_STREAMING = 2,
                          ACCESS_BUFFER = 3;
  public InputStream open(String name) throws IOException {
    return open(name, ACCESS_STREAMING);
  }
  public InputStream open(String name, int mode) throws IOException {
    if (mode < ACCESS_UNKNOWN || mode > ACCESS_BUFFER) throw new IllegalArgumentException("Bad access mode");
    String path = "assets/" + java.util.Objects.requireNonNull(name);
    var trace = AssetReadTrace.begin(path);
    return AssetReadTrace.opened(RuntimeEnvironment.host().openAsset(path, mode), trace);
  }
  public String[] list(String path) throws IOException {
    return RuntimeEnvironment.host().listAssets("assets/" + path);
  }
  public AssetFileDescriptor openFd(String name) throws IOException {
    return RuntimeEnvironment.host().openAssetFd("assets/" + java.util.Objects.requireNonNull(name));
  }
  public InputStream openNonAsset(String path) throws IOException {
    var trace = AssetReadTrace.begin(path);
    return AssetReadTrace.opened(RuntimeEnvironment.host().openAsset(
        java.util.Objects.requireNonNull(path), ACCESS_STREAMING), trace);
  }
  public void close() {} // Process-owned shared PackageAssets remains alive for native consumers.
}
