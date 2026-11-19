package android.hardware.usb;

/** An asynchronous transfer. Initialization fails, as no connection is ever open. */
public class UsbRequest {
  private UsbEndpoint endpoint;
  private Object clientData;
  public boolean initialize(UsbDeviceConnection connection, UsbEndpoint endpoint) {
    this.endpoint = endpoint;
    return false;
  }
  public void close() {}
  public UsbEndpoint getEndpoint() { return endpoint; }
  public Object getClientData() { return clientData; }
  public void setClientData(Object data) { clientData = data; }
  public boolean queue(java.nio.ByteBuffer buffer, int length) { return false; }
  public boolean queue(java.nio.ByteBuffer buffer) { return false; }
  public boolean cancel() { return false; }
}
