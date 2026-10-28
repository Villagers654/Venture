package android.os;

/** Binder peer death is a RemoteException; no remote Binder transport is supplied. */
public class DeadObjectException extends RemoteException {
  public DeadObjectException() {}
  public DeadObjectException(String message) { super(message); }
}
