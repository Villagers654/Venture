package android.hardware.usb;

/** An open device. UsbManager.openDevice never yields one in Venture, as no
 * Android USB host is exposed; every transfer fails as on a closed connection. */
public class UsbDeviceConnection {
  private final UsbDevice device;
  public UsbDeviceConnection(UsbDevice device) { this.device = device; }
  public void close() {}
  public int getFileDescriptor() { return -1; }
  public byte[] getRawDescriptors() { return null; }
  public boolean claimInterface(UsbInterface usbInterface, boolean force) { return false; }
  public boolean releaseInterface(UsbInterface usbInterface) { return false; }
  public boolean setInterface(UsbInterface usbInterface) { return false; }
  public boolean setConfiguration(UsbConfiguration configuration) { return false; }
  public int controlTransfer(int requestType, int request, int value, int index, byte[] buffer, int length, int timeout) {
    return -1;
  }
  public int controlTransfer(int requestType, int request, int value, int index, byte[] buffer, int offset, int length,
                             int timeout) {
    return -1;
  }
  public int bulkTransfer(UsbEndpoint endpoint, byte[] buffer, int length, int timeout) { return -1; }
  public int bulkTransfer(UsbEndpoint endpoint, byte[] buffer, int offset, int length, int timeout) { return -1; }
  public UsbRequest requestWait() { return null; }
  public UsbRequest requestWait(long timeout) throws java.util.concurrent.TimeoutException { return null; }
  public String getSerial() { return device == null ? null : device.getSerialNumber(); }
}
