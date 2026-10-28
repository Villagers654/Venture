package android.os;

/** Local Binder calls only. No Android system service or remote transport is advertised. */
public class Binder implements IBinder {
  private IInterface owner;
  private String descriptor;
  public synchronized void attachInterface(IInterface value, String name) {
    owner = value;
    descriptor = name;
  }
  public synchronized String getInterfaceDescriptor() { return descriptor; }
  public synchronized IInterface queryLocalInterface(String name) {
    return java.util.Objects.equals(descriptor, name) ? owner : null;
  }
  public boolean pingBinder() { return true; }
  public boolean isBinderAlive() { return true; }
  // AOSP Binder.java: local objects cannot die independently of their process;
  // death-recipient registration has no effect and unlink returns true.
  public void linkToDeath(DeathRecipient recipient, int flags) {}
  public boolean unlinkToDeath(DeathRecipient recipient, int flags) { return true; }
  public final boolean transact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
    if (data != null) data.setDataPosition(0);
    try { return onTransact(code, data, reply, flags); }
    finally { if (reply != null) reply.setDataPosition(0); }
  }
  protected boolean onTransact(int code, Parcel data, Parcel reply, int flags) throws RemoteException {
    if (code == INTERFACE_TRANSACTION) {
      if (reply != null) reply.writeString(getInterfaceDescriptor());
      return true;
    }
    return false;
  }
  public static int getCallingPid() { return Process.myPid(); }
  public static int getCallingUid() { return Process.myUid(); }
}
