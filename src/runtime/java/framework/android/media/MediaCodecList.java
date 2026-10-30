package android.media;

/** AOSP android.media.MediaCodecList over the decoders MediaCodec can create. */
public final class MediaCodecList {
  public static final int REGULAR_CODECS = 0, ALL_CODECS = 1;
  private static final MediaCodecInfo[] DECODERS = {
      new MediaCodecInfo("c2.android.avc.decoder", "video/avc"),
      new MediaCodecInfo("c2.android.hevc.decoder", "video/hevc"),
      new MediaCodecInfo("c2.android.vp9.decoder", "video/x-vnd.on2.vp9"),
      new MediaCodecInfo("c2.android.vp8.decoder", "video/x-vnd.on2.vp8"),
      new MediaCodecInfo("c2.android.aac.decoder", "audio/mp4a-latm"),
  };
  public MediaCodecList(int kind) {
    if (kind != REGULAR_CODECS && kind != ALL_CODECS) throw new IllegalArgumentException("Invalid kind " + kind);
  }
  public static int getCodecCount() { return DECODERS.length; }
  public static MediaCodecInfo getCodecInfoAt(int index) {
    if (index < 0 || index >= DECODERS.length) throw new IllegalArgumentException();
    return DECODERS[index];
  }
  public MediaCodecInfo[] getCodecInfos() { return DECODERS.clone(); }
  public String findDecoderForFormat(MediaFormat format) {
    String mime = format.getString(MediaFormat.KEY_MIME);
    for (var info : DECODERS) if (info.getSupportedTypes()[0].equalsIgnoreCase(mime)) return info.getName();
    return null;
  }
  public String findEncoderForFormat(MediaFormat format) { return null; }
}
