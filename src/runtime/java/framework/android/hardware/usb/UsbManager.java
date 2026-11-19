package android.hardware.usb;

/**
 * Context.USB_SERVICE. Venture exposes no Android USB host or accessory port
 * (PackageManager reports neither FEATURE_USB_HOST nor FEATURE_USB_ACCESSORY),
 * so it answers as such a device does: no devices, no accessories, no
 * permissions, and a permission request is answered as denied.
 */
public class UsbManager {
  public static final String ACTION_USB_STATE = "android.hardware.usb.action.USB_STATE",
      ACTION_USB_DEVICE_ATTACHED = "android.hardware.usb.action.USB_DEVICE_ATTACHED",
      ACTION_USB_DEVICE_DETACHED = "android.hardware.usb.action.USB_DEVICE_DETACHED",
      ACTION_USB_ACCESSORY_ATTACHED = "android.hardware.usb.action.USB_ACCESSORY_ATTACHED",
      ACTION_USB_ACCESSORY_DETACHED = "android.hardware.usb.action.USB_ACCESSORY_DETACHED",
      ACTION_USB_ACCESSORY_HANDSHAKE = "android.hardware.usb.action.USB_ACCESSORY_HANDSHAKE";
  public static final String EXTRA_DEVICE = "device", EXTRA_ACCESSORY = "accessory",
      EXTRA_PERMISSION_GRANTED = "permission";
  public static final String USB_CONNECTED = "connected", USB_CONFIGURED = "configured",
      USB_FUNCTION_MTP = "mtp", USB_FUNCTION_PTP = "ptp", USB_FUNCTION_MIDI = "midi";
  private final android.content.Context context;
  public UsbManager(android.content.Context context) { this.context = context; }
  /** A new map on each call, as AOSP builds one from the service's list. */
  public java.util.HashMap<String, UsbDevice> getDeviceList() { return new java.util.HashMap<>(); }
  public UsbDeviceConnection openDevice(UsbDevice device) { return null; }
  /** AOSP returns null, not an empty array, when no accessory is attached. */
  public UsbAccessory[] getAccessoryList() { return null; }
  public android.os.ParcelFileDescriptor openAccessory(UsbAccessory accessory) { return null; }
  public boolean hasPermission(UsbDevice device) { return false; }
  public boolean hasPermission(UsbAccessory accessory) { return false; }
  public void requestPermission(UsbDevice device, android.app.PendingIntent pi) { deny(pi, EXTRA_DEVICE, device); }
  public void requestPermission(UsbAccessory accessory, android.app.PendingIntent pi) {
    deny(pi, EXTRA_ACCESSORY, accessory);
  }
  // The result AOSP's permission dialog sends when the user declines.
  private void deny(android.app.PendingIntent pi, String key, android.os.Parcelable target) {
    if (pi == null) return;
    var result = new android.content.Intent();
    result.putExtra(key, target);
    result.putExtra(EXTRA_PERMISSION_GRANTED, false);
    try {
      pi.send(context, 0, result);
    } catch (android.app.PendingIntent.CanceledException e) {
      android.util.Log.w("UsbManager", "permission result not delivered: " + e);
    }
  }
  public boolean isFunctionEnabled(String function) { return false; }
  public long getCurrentFunctions() { return 0; }
}
