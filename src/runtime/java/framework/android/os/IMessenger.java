package android.os;

/** In-process adapter for AOSP android-12.1.0_r27 IMessenger.aidl (oneway send). */
interface IMessenger extends IInterface {
  void send(Message message) throws RemoteException;

  abstract class Stub extends Binder implements IMessenger {
    private static final String DESCRIPTOR = "android.os.IMessenger";
    private static final int SEND = IBinder.FIRST_CALL_TRANSACTION;
    Stub() { attachInterface(this, DESCRIPTOR); }
    public IBinder asBinder() { return this; }
    static IMessenger asInterface(IBinder binder) {
      if (binder == null) return null;
      IInterface local = binder.queryLocalInterface(DESCRIPTOR);
      if (local instanceof IMessenger messenger) return messenger;
      return new IMessenger() {
        public IBinder asBinder() { return binder; }
        public void send(Message message) throws RemoteException {
          Parcel data = Parcel.obtain();
          try {
            data.writeInterfaceToken(DESCRIPTOR);
            data.writeInt(message == null ? 0 : 1);
            if (message != null) message.writeToParcel(data, 0);
            if (!binder.transact(SEND, data, null, IBinder.FLAG_ONEWAY))
              throw new RemoteException("Binder does not implement IMessenger.send");
          } finally { data.recycle(); }
        }
      };
    }
    protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
        throws RemoteException {
      if (code != SEND) return super.onTransact(code, data, reply, flags);
      data.enforceInterface(DESCRIPTOR);
      send(data.readInt() == 0 ? null : Message.CREATOR.createFromParcel(data));
      return true;
    }
  }
}
