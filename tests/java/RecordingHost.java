import com.venture.runtime.Host;
import java.util.*;
import java.util.zip.*;

public final class RecordingHost implements Host, AutoCloseable {
  private final ZipFile zip;
  public final List<String> events = java.util.Collections.synchronizedList(new ArrayList<>());
  public int networkType = -2;
  private final java.util.concurrent.locks.ReentrantLock exitDecision =
      new java.util.concurrent.locks.ReentrantLock();
  public void beginProcessExit() { exitDecision.lock(); }
  public void endProcessExit() { exitDecision.unlock(); }
  public java.util.concurrent.CountDownLatch restartClaimed, publishRestart;
  public int restarts;
  public byte[] launchIntent;
  public void restartProcess() { restarts++; throw new ThreadDeath(); }
  public void restartProcess(byte[] payload) {
    if (restartClaimed != null) {
      restartClaimed.countDown();
      try { if (!publishRestart.await(5, java.util.concurrent.TimeUnit.SECONDS))
        throw new AssertionError("restart publication not released"); }
      catch (InterruptedException error) { throw new AssertionError(error); }
    }
    launchIntent = payload.clone(); restartProcess();
  }
  public byte[] coldLaunchIntent() { var result = launchIntent; launchIntent = null; return result; }
  public void exitProcess(int status) { events.add("exit " + status); throw new ThreadDeath(); }
  public int activeNetworkType() { return networkType; }
  private long surface;
  public RecordingHost(String apk) throws Exception {
    zip = new ZipFile(apk);
  }
  public int guestTid() {
    return Math.toIntExact(Thread.currentThread().threadId());
  }
  public void bindClass(Class<?> cls) {} // Guest native methods are rewritten only in this harness.
  public void log(int priority, String tag, String message) {
    System.out.println("FRAMEWORK " + tag + ": " + message);
    events.add(tag + ": " + message);
  }
  public void loadLibrary(String name, ClassLoader loader) {
    System.out.println("LOAD " + name);
    events.add("load " + name);
  }
  public long createSurface(int width, int height, int format) {
    events.add("surface " + width + "x" + height);
    return ++surface;
  }
  public void releaseSurface(long handle) {
    events.add("release " + handle);
  }
  public long createNativeActivity(
      android.app.NativeActivity activity, String library, String entry) {
    TestNatives.enter("NativeActivity." + library + "!" + entry);
    return 1;
  }
  public void nativeActivityEvent(long handle, int event, long surface) {
    events.add("activity event " + event);
  }
  public byte[] readAsset(String path) {
    try {
      var entry = zip.getEntry(path);
      if (entry == null)
        return null;
      try (var in = zip.getInputStream(entry)) {
        return in.readAllBytes();
      }
    } catch (Exception e) {
      throw new IllegalStateException(e);
    }
  }
  public String[] listAssets(String path) {
    String prefix = path.endsWith("/") ? path : path + "/";
    return zip.stream()
        .map(ZipEntry::getName)
        .filter(n -> n.startsWith(prefix))
        .map(n -> n.substring(prefix.length()).split("/", 2)[0])
        .filter(n -> !n.isEmpty())
        .distinct()
        .sorted()
        .toArray(String[] ::new);
  }
  public void close() throws Exception {
    zip.close();
  }
}
