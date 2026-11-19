package android.hardware.usb;

/** USB descriptor constants, as android.hardware.usb.UsbConstants. */
public final class UsbConstants {
  private UsbConstants() {}
  public static final int USB_ENDPOINT_DIR_MASK = 0x80, USB_DIR_OUT = 0, USB_DIR_IN = 0x80;
  public static final int USB_ENDPOINT_NUMBER_MASK = 0x0f, USB_ENDPOINT_XFERTYPE_MASK = 0x03;
  public static final int USB_ENDPOINT_XFER_CONTROL = 0, USB_ENDPOINT_XFER_ISOC = 1, USB_ENDPOINT_XFER_BULK = 2,
                          USB_ENDPOINT_XFER_INT = 3;
  public static final int USB_TYPE_MASK = 0x03 << 5, USB_TYPE_STANDARD = 0, USB_TYPE_CLASS = 0x01 << 5,
                          USB_TYPE_VENDOR = 0x02 << 5, USB_TYPE_RESERVED = 0x03 << 5;
  public static final int USB_CLASS_PER_INTERFACE = 0, USB_CLASS_AUDIO = 1, USB_CLASS_COMM = 2, USB_CLASS_HID = 3,
                          USB_CLASS_PHYSICA = 5, USB_CLASS_STILL_IMAGE = 6, USB_CLASS_PRINTER = 7,
                          USB_CLASS_MASS_STORAGE = 8, USB_CLASS_HUB = 9, USB_CLASS_CDC_DATA = 0x0a,
                          USB_CLASS_CSCID = 0x0b, USB_CLASS_CONTENT_SEC = 0x0d, USB_CLASS_VIDEO = 0x0e,
                          USB_CLASS_WIRELESS_CONTROLLER = 0xe0, USB_CLASS_MISC = 0xef, USB_CLASS_APP_SPEC = 0xfe,
                          USB_CLASS_VENDOR_SPEC = 0xff;
  public static final int USB_INTERFACE_SUBCLASS_BOOT = 1, USB_SUBCLASS_VENDOR_SPEC = 0xff;
}
