package android.os;
public class RemoteException extends Exception {
  public RemoteException() {}
  public RemoteException(String message) { super(message); }
  public RuntimeException rethrowAsRuntimeException() { throw new RuntimeException(this); }
  public RuntimeException rethrowFromSystemServer() { throw new RuntimeException(this); }
}
