package android.os;
public final class Message implements Parcelable {
  public int what, arg1, arg2;
  public Object obj;
  public Messenger replyTo;
  public int sendingUid = -1;
  private int workSourceUid = -1;
  Handler target;
  Runnable callback;
  long when, sequence;
  Bundle data;
  boolean inUse;
  private boolean asynchronous;
  public boolean isAsynchronous() {
    return asynchronous;
  }
  public void setAsynchronous(boolean value) {
    asynchronous = value;
  }
  public static Message obtain() {
    return new Message();
  }
  public static Message obtain(Message from) {
    Message m = obtain();
    m.what = from.what;
    m.arg1 = from.arg1;
    m.arg2 = from.arg2;
    m.obj = from.obj;
    m.replyTo = from.replyTo;
    m.sendingUid = from.sendingUid;
    m.workSourceUid = from.workSourceUid;
    m.target = from.target;
    m.callback = from.callback;
    m.data = from.data == null ? null : new Bundle(from.data);
    m.asynchronous = from.asynchronous;
    return m;
  }
  public static Message obtain(Handler h) {
    Message m = obtain();
    m.target = h;
    return m;
  }
  public static Message obtain(Handler h, int w) {
    Message m = obtain(h);
    m.what = w;
    return m;
  }
  public static Message obtain(Handler h, int w, Object o) {
    Message m = obtain(h, w);
    m.obj = o;
    return m;
  }
  public static Message obtain(Handler h, int w, int a, int b) {
    Message m = obtain(h, w);
    m.arg1 = a;
    m.arg2 = b;
    return m;
  }
  public static Message obtain(Handler h, int w, int a, int b, Object o) {
    Message m = obtain(h, w, a, b);
    m.obj = o;
    return m;
  }
  public static Message obtain(Handler h, Runnable r) {
    Message m = obtain(h);
    m.callback = r;
    return m;
  }
  public Handler getTarget() {
    return target;
  }
  public void setTarget(Handler h) {
    target = h;
  }
  public Runnable getCallback() {
    return callback;
  }
  public long getWhen() {
    return when;
  }
  public Bundle getData() {
    if (data == null)
      data = new Bundle();
    return data;
  }
  public void setData(Bundle b) {
    data = b;
  }
  public void sendToTarget() {
    target.sendMessage(this);
  }
  public void recycle() {
    if (inUse)
      throw new IllegalStateException("Message in use");
  }
  // AOSP Message.java field order; Parcel stores values in-process, not wire bytes.
  public int describeContents() { return 0; }
  public void writeToParcel(Parcel dest, int flags) {
    if (callback != null) throw new RuntimeException("Cannot parcel a Message callback");
    if (obj != null && !(obj instanceof Parcelable))
      throw new RuntimeException("Message object must implement Parcelable");
    dest.writeInt(what);
    dest.writeInt(arg1);
    dest.writeInt(arg2);
    dest.writeInt(obj == null ? 0 : 1);
    if (obj != null) dest.writeParcelable((Parcelable) obj, flags);
    dest.writeLong(when);
    dest.writeBundle(data);
    Messenger.writeMessengerOrNullToParcel(replyTo, dest);
    dest.writeInt(sendingUid);
    dest.writeInt(workSourceUid);
  }
  public static final Parcelable.Creator<Message> CREATOR = new Parcelable.Creator<>() {
    public Message createFromParcel(Parcel source) {
      Message m = obtain();
      m.what = source.readInt();
      m.arg1 = source.readInt();
      m.arg2 = source.readInt();
      if (source.readInt() != 0) m.obj = source.readParcelable(Message.class.getClassLoader());
      m.when = source.readLong();
      m.data = source.readBundle();
      m.replyTo = Messenger.readMessengerOrNullFromParcel(source);
      m.sendingUid = source.readInt();
      m.workSourceUid = source.readInt();
      return m;
    }
    public Message[] newArray(int size) { return new Message[size]; }
  };
}
