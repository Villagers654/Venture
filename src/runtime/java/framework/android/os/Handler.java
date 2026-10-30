package android.os;
public class Handler {
  public interface Callback {
    boolean handleMessage(Message message);
  }
  private final Looper looper;
  private final Callback callback;
  private final boolean asynchronous;
  private IMessenger messenger;
  // AOSP Handler.java: every Messenger for a Handler shares one Binder identity.
  final IMessenger getIMessenger() {
    synchronized (looper.queue) {
      if (messenger == null) messenger = new IMessenger.Stub() {
        public void send(Message message) {
          message.sendingUid = Binder.getCallingUid();
          Handler.this.sendMessage(message);
        }
      };
      return messenger;
    }
  }
  public static Handler createAsync(Looper looper) {
    return new Handler(looper, null, true);
  }
  public static Handler createAsync(Looper looper, Callback callback) {
    return new Handler(looper, java.util.Objects.requireNonNull(callback), true);
  }
  public Handler() {
    this(Looper.myLooper(), null);
  }
  public Handler(Callback c) {
    this(Looper.myLooper(), c);
  }
  public Handler(Looper l) {
    this(l, null);
  }
  public Handler(Looper l, Callback c) {
    this(l, c, false);
  }
  public Handler(Looper l, Callback c, boolean async) {
    if (l == null)
      throw new IllegalStateException("Handler needs a Looper");
    looper = l;
    callback = c;
    asynchronous = async;
  }
  public void handleMessage(Message m) {}
  public void dispatchMessage(Message m) {
    if (m.callback != null)
      m.callback.run();
    else if (callback == null || !callback.handleMessage(m))
      handleMessage(m);
  }
  public Looper getLooper() {
    return looper;
  }
  public boolean sendMessage(Message m) {
    return sendMessageDelayed(m, 0);
  }
  public boolean sendMessageDelayed(Message m, long delay) {
    return sendMessageAtTime(m, SystemClock.uptimeMillis() + Math.max(0, delay));
  }
  public boolean sendMessageAtTime(Message m, long time) {
    m.target = this;
    if (asynchronous)
      m.setAsynchronous(true);
    return looper.queue.enqueue(m, time);
  }
  public final boolean sendMessageAtFrontOfQueue(Message m) {
    return sendMessageAtTime(m, 0);
  }
  public final boolean sendEmptyMessageAtTime(int what, long uptimeMillis) {
    return sendMessageAtTime(obtainMessage(what), uptimeMillis);
  }
  public boolean sendEmptyMessage(int w) {
    return sendMessage(obtainMessage(w));
  }
  public boolean sendEmptyMessageDelayed(int w, long d) {
    return sendMessageDelayed(obtainMessage(w), d);
  }
  public boolean post(Runnable r) {
    return sendMessage(Message.obtain(this, r));
  }
  public boolean postDelayed(Runnable r, long d) {
    return sendMessageDelayed(Message.obtain(this, r), d);
  }
  public final boolean postAtFrontOfQueue(Runnable r) {
    return sendMessageAtTime(Message.obtain(this, r), 0);
  }
  @Override public String toString() {
    return "Handler (" + getClass().getName() + ") {" + Integer.toHexString(System.identityHashCode(this)) + "}";
  }
  public boolean postAtTime(Runnable r, long t) {
    return sendMessageAtTime(Message.obtain(this, r), t);
  }
  public boolean postAtTime(Runnable r, Object token, long t) {
    Message m = Message.obtain(this, r);
    m.obj = token;
    return sendMessageAtTime(m, t);
  }
  public void removeCallbacks(Runnable r) {
    looper.queue.remove(this, r, null, null);
  }
  public void removeCallbacks(Runnable r, Object token) {
    looper.queue.remove(this, r, token, null);
  }
  public void removeCallbacksAndMessages(Object token) {
    looper.queue.remove(this, null, token, null);
  }
  public void removeMessages(int w) {
    looper.queue.remove(this, null, null, w);
  }
  public void removeMessages(int w, Object o) {
    looper.queue.remove(this, null, o, w);
  }
  public boolean hasMessages(int w) {
    return looper.queue.has(this, w, null);
  }
  public boolean hasMessages(int w, Object o) {
    return looper.queue.has(this, w, o);
  }
  public Message obtainMessage() {
    return Message.obtain(this);
  }
  public Message obtainMessage(int w) {
    return Message.obtain(this, w);
  }
  public Message obtainMessage(int w, Object o) {
    return Message.obtain(this, w, o);
  }
  public Message obtainMessage(int w, int a, int b) {
    return Message.obtain(this, w, a, b);
  }
  public Message obtainMessage(int w, int a, int b, Object o) {
    return Message.obtain(this, w, a, b, o);
  }
}
