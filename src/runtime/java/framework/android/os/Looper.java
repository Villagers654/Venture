package android.os;
public final class Looper {
  private static final ThreadLocal<Looper> CURRENT = new ThreadLocal<>();
  private static volatile Looper main;
  final MessageQueue queue = new MessageQueue();
  private final Thread thread = Thread.currentThread();
  public static void prepare() {
    if (CURRENT.get() != null)
      throw new IllegalStateException("Only one Looper per thread");
    CURRENT.set(new Looper());
  }
  public static synchronized void prepareMainLooper() {
    if (main != null)
      throw new IllegalStateException("Main Looper already prepared");
    prepare();
    main = myLooper();
  }
  public static Looper myLooper() {
    return CURRENT.get();
  }
  public static Looper getMainLooper() {
    return main;
  }
  public static MessageQueue myQueue() {
    return myLooper().queue;
  }
  public MessageQueue getQueue() {
    return queue;
  }
  public Thread getThread() {
    return thread;
  }
  public boolean isCurrentThread() {
    return thread == Thread.currentThread();
  }
  public static void loop() {
    Looper l = myLooper();
    if (l == null)
      throw new IllegalStateException("No Looper");
    try {
      for (Message m; (m = l.queue.next()) != null;) m.target.dispatchMessage(m);
    } finally {
      l.queue.quit(false);
    }
  }
  public void quit() {
    queue.quit(false);
  }
  public void quitSafely() {
    queue.quit(true);
  }
}
