public final class MenuQuitTest {
  private static java.io.BufferedOutputStream save;
  private static final java.util.concurrent.CountDownLatch destroyRelease = new java.util.concurrent.CountDownLatch(1);
  private static Thread destroy;
  public static void bufferedSave(String path) throws Exception {
    save = new java.io.BufferedOutputStream(new java.io.FileOutputStream(path));
    save.write("pause-buffered-save".getBytes(java.nio.charset.StandardCharsets.UTF_8));
  }
  public static void closeSave() throws Exception { save.close(); save = null; }
  public static void crashWhileDestroyBlocked() throws Exception {
    var install = com.venture.runtime.AppMain.class.getDeclaredMethod("installFailureHandler");
    install.setAccessible(true); install.invoke(null);
    var entered = new java.util.concurrent.CountDownLatch(1);
    destroy = new Thread(() -> {
      entered.countDown();
      try { destroyRelease.await(); }
      catch (InterruptedException e) { Thread.currentThread().interrupt(); }
    }, "fixture-destroy-wait");
    destroy.setDaemon(true); destroy.start(); entered.await();
    Thread crash = new Thread(() -> { throw new IllegalStateException("expected fixture background failure"); }, "fixture-background-crash");
    crash.start(); crash.join();
  }
  public static void releaseDestroy() throws Exception { destroyRelease.countDown(); destroy.join(); }
  public static volatile long counter;
  private static volatile boolean stop;
  private static Thread worker;
  public static void start() {
    worker = new Thread(() -> { while (!stop) { ++counter; Thread.onSpinWait(); } }, "menu-quit-counter");
    worker.setDaemon(true);
    worker.start();
  }
  public static void stop() throws InterruptedException { stop = true; worker.join(); }
  // Game JNI code that blocks, as GameActivity.terminateNativeCode waits for android_main.
  public static native void parkNative();
  private static Thread parked;
  public static void startParked() {
    parked = new Thread(MenuQuitTest::parkNative, "fixture-parked-native");
    parked.setDaemon(true);
    parked.start();
  }
  public static void joinParked() throws InterruptedException { parked.join(); }
  // A NativeActivity callback that blocks, as UE4's onStop waits for its game thread.
  private static Thread activityEvent;
  public static void startActivityEvent() {
    var host = new com.venture.runtime.NativeHost();
    activityEvent = new Thread(() -> host.nativeActivityEvent(0, 0, 0), "fixture-activity-event");
    activityEvent.setDaemon(true);
    activityEvent.start();
  }
  public static void joinActivityEvent() throws InterruptedException { activityEvent.join(); }
  // A save whose fsync is slow (the test replaces FileDescriptor.sync0).
  private static Thread syncing;
  public static void startSync(String path) {
    syncing = new Thread(() -> {
      try (var out = new java.io.FileOutputStream(path)) { out.write(1); out.getFD().sync(); }
      catch (java.io.IOException e) { throw new java.io.UncheckedIOException(e); }
    }, "fixture-slow-sync");
    syncing.setDaemon(true);
    syncing.start();
  }
  public static void joinSync() throws InterruptedException { syncing.join(); }
  // An HTTP client's timed socket read waiting for a peer that never answers (Net.poll).
  private static java.net.ServerSocket netServer;
  private static java.net.Socket netPeer;
  private static Thread netWait;
  public static void startNetWait() throws Exception {
    netServer = new java.net.ServerSocket(0, 1, java.net.InetAddress.getLoopbackAddress());
    var client = new java.net.Socket(java.net.InetAddress.getLoopbackAddress(), netServer.getLocalPort());
    netPeer = netServer.accept();
    client.setSoTimeout(60000);
    netWait = new Thread(() -> {
      try (client) { client.getInputStream().read(); }
      catch (java.io.IOException e) { throw new java.io.UncheckedIOException(e); }
    }, "fixture-net-wait");
    netWait.setDaemon(true);
    netWait.start();
  }
  public static boolean netWaiting() {
    var stack = netWait.getStackTrace();
    return stack.length > 0 && stack[0].getClassName().equals("sun.nio.ch.Net") && stack[0].getMethodName().equals("poll");
  }
  public static void joinNetWait() throws Exception {
    netPeer.close(); netWait.join(); netServer.close();
  }
}
