package android.media;

/** A default route provided by Venture's native PCM mixer. */
public final class AudioDeviceInfo {
  // AOSP AudioDeviceInfo.java public connector constants, not claimed devices.
  public static final int TYPE_UNKNOWN = 0, TYPE_BUILTIN_EARPIECE = 1, TYPE_BUILTIN_SPEAKER = 2,
      TYPE_WIRED_HEADSET = 3, TYPE_WIRED_HEADPHONES = 4, TYPE_LINE_ANALOG = 5, TYPE_LINE_DIGITAL = 6,
      TYPE_BLUETOOTH_SCO = 7, TYPE_BLUETOOTH_A2DP = 8, TYPE_HDMI = 9, TYPE_HDMI_ARC = 10,
      TYPE_USB_DEVICE = 11, TYPE_USB_ACCESSORY = 12, TYPE_DOCK = 13, TYPE_FM = 14,
      TYPE_BUILTIN_MIC = 15, TYPE_FM_TUNER = 16, TYPE_TV_TUNER = 17, TYPE_TELEPHONY = 18,
      TYPE_AUX_LINE = 19, TYPE_IP = 20, TYPE_BUS = 21, TYPE_USB_HEADSET = 22,
      TYPE_HEARING_AID = 23, TYPE_BUILTIN_SPEAKER_SAFE = 24, TYPE_REMOTE_SUBMIX = 25,
      TYPE_BLE_HEADSET = 26, TYPE_BLE_SPEAKER = 27;
  private final int id;
  private final boolean input;
  private final String name;
  public AudioDeviceInfo(int id, boolean input, String name) {
    this.id = id; this.input = input; this.name = name;
  }
  public int getId() { return id; }
  public boolean isSource() { return input; }
  public boolean isSink() { return !input; }
  public CharSequence getProductName() { return name; }
  // The portable backend exposes default routing but not physical connector type.
  public int getType() { return TYPE_UNKNOWN; }
  public String getAddress() { return ""; }
  public int[] getChannelCounts() { return new int[]{1, 2}; }
  public int[] getChannelMasks() { return input ? new int[]{16, 12} : new int[]{4, 12}; }
  public int[] getChannelIndexMasks() { return new int[]{1, 3}; }
  public int[] getEncodings() { return new int[]{2, 3, 4, 21, 22}; }
  public int[] getSampleRates() {
    return new int[]{8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000, 176400, 192000};
  }
}
