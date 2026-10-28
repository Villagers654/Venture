package android.os;
public interface IBinder {
  interface DeathRecipient { void binderDied(); }
  int FIRST_CALL_TRANSACTION = 1, LAST_CALL_TRANSACTION = 0x00ffffff, FLAG_ONEWAY = 1;
  int INTERFACE_TRANSACTION = 0x5f4e5446;
  String getInterfaceDescriptor();
  boolean pingBinder();
  boolean isBinderAlive();
  IInterface queryLocalInterface(String descriptor);
  boolean transact(int code, Parcel data, Parcel reply, int flags) throws RemoteException;
  void linkToDeath(DeathRecipient recipient, int flags) throws RemoteException;
  boolean unlinkToDeath(DeathRecipient recipient, int flags);
}
