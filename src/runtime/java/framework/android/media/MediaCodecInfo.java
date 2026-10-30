package android.media;

/**
 * Android API 32 codec metadata. MediaCodecList lists the host's software
 * decoders (FFmpeg) under AOSP's c2.android names; there are no encoders.
 */
public final class MediaCodecInfo {
  private final String name;
  private final String type;
  MediaCodecInfo(String name, String type) { this.name = name; this.type = type; }
  public String getName() { return name; }
  public String getCanonicalName() { return name; }
  public boolean isAlias() { return false; }
  public boolean isEncoder() { return false; }
  public boolean isVendor() { return false; }
  public boolean isSoftwareOnly() { return true; }
  public boolean isHardwareAccelerated() { return false; }
  public String[] getSupportedTypes() { return new String[] {type}; }
  public CodecCapabilities getCapabilitiesForType(String mime) {
    if (!type.equalsIgnoreCase(mime)) throw new IllegalArgumentException("codec does not support type");
    return CodecCapabilities.forType(type);
  }
  private static CodecProfileLevel level(int profile, int level) {
    var value = new CodecProfileLevel();
    value.profile = profile;
    value.level = level;
    return value;
  }
  public static final class VideoCapabilities {
    private static final android.util.Range<Integer> SIZES = android.util.Range.create(2, 8192),
        RATES = android.util.Range.create(0, 960), BITRATES = android.util.Range.create(1, 200_000_000);
    VideoCapabilities() {}
    public android.util.Range<Integer> getBitrateRange() { return BITRATES; }
    public android.util.Range<Integer> getSupportedWidths() { return SIZES; }
    public android.util.Range<Integer> getSupportedHeights() { return SIZES; }
    public android.util.Range<Integer> getSupportedFrameRates() { return RATES; }
    public android.util.Range<Integer> getSupportedWidthsFor(int height) {
      if (!SIZES.contains(height)) throw new IllegalArgumentException("unsupported height");
      return SIZES;
    }
    public android.util.Range<Integer> getSupportedHeightsFor(int width) {
      if (!SIZES.contains(width)) throw new IllegalArgumentException("unsupported width");
      return SIZES;
    }
    public android.util.Range<Double> getSupportedFrameRatesFor(int width, int height) {
      if (!isSizeSupported(width, height)) throw new IllegalArgumentException("unsupported size");
      return android.util.Range.create(0.0, 960.0);
    }
    public int getWidthAlignment() { return 2; }
    public int getHeightAlignment() { return 2; }
    public boolean isSizeSupported(int width, int height) {
      return SIZES.contains(width) && SIZES.contains(height) && width % 2 == 0 && height % 2 == 0;
    }
    public boolean areSizeAndRateSupported(int width, int height, double frameRate) {
      return isSizeSupported(width, height) && frameRate >= 0 && frameRate <= 960;
    }
  }
  public static final class AudioCapabilities {
    AudioCapabilities() {}
    public android.util.Range<Integer> getBitrateRange() { return android.util.Range.create(8000, 960_000); }
    public int[] getSupportedSampleRates() { return new int[] {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000}; }
    public android.util.Range<Integer>[] getSupportedSampleRateRanges() {
      @SuppressWarnings("unchecked") var ranges = (android.util.Range<Integer>[]) new android.util.Range<?>[] {android.util.Range.create(8000, 96000)};
      return ranges;
    }
    public boolean isSampleRateSupported(int rate) { return rate >= 8000 && rate <= 96000; }
    // Venture's host decoder outputs mono or stereo PCM.
    public int getMaxInputChannelCount() { return 2; }
    public int getMinInputChannelCount() { return 1; }
  }
  public static final class CodecCapabilities {
    public static final int COLOR_Format12bitRGB444 = 3;
    public static final int COLOR_Format16bitARGB1555 = 5;
    public static final int COLOR_Format16bitARGB4444 = 4;
    public static final int COLOR_Format16bitBGR565 = 7;
    public static final int COLOR_Format16bitRGB565 = 6;
    public static final int COLOR_Format18BitBGR666 = 41;
    public static final int COLOR_Format18bitARGB1665 = 9;
    public static final int COLOR_Format18bitRGB666 = 8;
    public static final int COLOR_Format19bitARGB1666 = 10;
    public static final int COLOR_Format24BitABGR6666 = 43;
    public static final int COLOR_Format24BitARGB6666 = 42;
    public static final int COLOR_Format24bitARGB1887 = 13;
    public static final int COLOR_Format24bitBGR888 = 12;
    public static final int COLOR_Format24bitRGB888 = 11;
    public static final int COLOR_Format25bitARGB1888 = 14;
    public static final int COLOR_Format32bitABGR8888 = 2130747392;
    public static final int COLOR_Format32bitARGB8888 = 16;
    public static final int COLOR_Format32bitBGRA8888 = 15;
    public static final int COLOR_Format8bitRGB332 = 2;
    public static final int COLOR_FormatCbYCrY = 27;
    public static final int COLOR_FormatCrYCbY = 28;
    public static final int COLOR_FormatL16 = 36;
    public static final int COLOR_FormatL2 = 33;
    public static final int COLOR_FormatL24 = 37;
    public static final int COLOR_FormatL32 = 38;
    public static final int COLOR_FormatL4 = 34;
    public static final int COLOR_FormatL8 = 35;
    public static final int COLOR_FormatMonochrome = 1;
    public static final int COLOR_FormatRGBAFlexible = 2134288520;
    public static final int COLOR_FormatRGBFlexible = 2134292616;
    public static final int COLOR_FormatRawBayer10bit = 31;
    public static final int COLOR_FormatRawBayer8bit = 30;
    public static final int COLOR_FormatRawBayer8bitcompressed = 32;
    public static final int COLOR_FormatSurface = 2130708361;
    public static final int COLOR_FormatYCbYCr = 25;
    public static final int COLOR_FormatYCrYCb = 26;
    public static final int COLOR_FormatYUV411PackedPlanar = 18;
    public static final int COLOR_FormatYUV411Planar = 17;
    public static final int COLOR_FormatYUV420Flexible = 2135033992;
    public static final int COLOR_FormatYUV420PackedPlanar = 20;
    public static final int COLOR_FormatYUV420PackedSemiPlanar = 39;
    public static final int COLOR_FormatYUV420Planar = 19;
    public static final int COLOR_FormatYUV420SemiPlanar = 21;
    public static final int COLOR_FormatYUV422Flexible = 2135042184;
    public static final int COLOR_FormatYUV422PackedPlanar = 23;
    public static final int COLOR_FormatYUV422PackedSemiPlanar = 40;
    public static final int COLOR_FormatYUV422Planar = 22;
    public static final int COLOR_FormatYUV422SemiPlanar = 24;
    public static final int COLOR_FormatYUV444Flexible = 2135181448;
    public static final int COLOR_FormatYUV444Interleaved = 29;
    public static final int COLOR_QCOM_FormatYUV420SemiPlanar = 2141391872;
    public static final int COLOR_TI_FormatYUV420PackedSemiPlanar = 2130706688;
    public static final java.lang.String FEATURE_AdaptivePlayback = "adaptive-playback";
    public static final java.lang.String FEATURE_DynamicTimestamp = "dynamic-timestamp";
    public static final java.lang.String FEATURE_FrameParsing = "frame-parsing";
    public static final java.lang.String FEATURE_IntraRefresh = "intra-refresh";
    public static final java.lang.String FEATURE_LowLatency = "low-latency";
    public static final java.lang.String FEATURE_MultipleFrames = "multiple-frames";
    public static final java.lang.String FEATURE_PartialFrame = "partial-frame";
    public static final java.lang.String FEATURE_QpBounds = "qp-bounds";
    public static final java.lang.String FEATURE_SecurePlayback = "secure-playback";
    public static final java.lang.String FEATURE_TunneledPlayback = "tunneled-playback";
    public int[] colorFormats = new int[0];
    public CodecProfileLevel[] profileLevels = new CodecProfileLevel[0];
    private String mime = "";
    public CodecCapabilities() {}
    static CodecCapabilities forType(String mime) {
      var c = new CodecCapabilities();
      c.mime = mime;
      if (mime.startsWith("video/")) c.colorFormats = new int[] {COLOR_FormatYUV420Flexible};
      c.profileLevels = switch (mime) {
        case "video/avc" -> new CodecProfileLevel[] {
            level(CodecProfileLevel.AVCProfileConstrainedBaseline, CodecProfileLevel.AVCLevel62),
            level(CodecProfileLevel.AVCProfileBaseline, CodecProfileLevel.AVCLevel62),
            level(CodecProfileLevel.AVCProfileMain, CodecProfileLevel.AVCLevel62),
            level(CodecProfileLevel.AVCProfileConstrainedHigh, CodecProfileLevel.AVCLevel62),
            level(CodecProfileLevel.AVCProfileHigh, CodecProfileLevel.AVCLevel62)};
        case "video/hevc" -> new CodecProfileLevel[] {
            level(CodecProfileLevel.HEVCProfileMain, CodecProfileLevel.HEVCHighTierLevel62),
            level(CodecProfileLevel.HEVCProfileMain10, CodecProfileLevel.HEVCHighTierLevel62)};
        case "video/x-vnd.on2.vp9" -> new CodecProfileLevel[] {
            level(CodecProfileLevel.VP9Profile0, CodecProfileLevel.VP9Level62),
            level(CodecProfileLevel.VP9Profile2, CodecProfileLevel.VP9Level62)};
        case "video/x-vnd.on2.vp8" -> new CodecProfileLevel[] {
            level(CodecProfileLevel.VP8ProfileMain, CodecProfileLevel.VP8Level_Version0)};
        case "audio/mp4a-latm" -> new CodecProfileLevel[] {
            level(CodecProfileLevel.AACObjectLC, 0), level(CodecProfileLevel.AACObjectHE, 0),
            level(CodecProfileLevel.AACObjectHE_PS, 0)};
        default -> new CodecProfileLevel[0];
      };
      return c;
    }
    public String getMimeType() { return mime; }
    public int getMaxSupportedInstances() { return 32; }
    public boolean isFeatureSupported(String feature) {
      return mime.startsWith("video/") && FEATURE_AdaptivePlayback.equals(feature);
    }
    public boolean isFeatureRequired(String feature) { return false; }
    public boolean isFormatSupported(MediaFormat format) {
      return format != null && mime.equalsIgnoreCase(format.getString(MediaFormat.KEY_MIME));
    }
    public VideoCapabilities getVideoCapabilities() { return mime.startsWith("video/") ? new VideoCapabilities() : null; }
    public AudioCapabilities getAudioCapabilities() { return mime.startsWith("audio/") ? new AudioCapabilities() : null; }
    public MediaFormat getDefaultFormat() { var format = new MediaFormat(); format.setString(MediaFormat.KEY_MIME, mime); return format; }
  }
  public static final class CodecProfileLevel {
    public static final int AACObjectELD = 39;
    public static final int AACObjectERLC = 17;
    public static final int AACObjectERScalable = 20;
    public static final int AACObjectHE = 5;
    public static final int AACObjectHE_PS = 29;
    public static final int AACObjectLC = 2;
    public static final int AACObjectLD = 23;
    public static final int AACObjectLTP = 4;
    public static final int AACObjectMain = 1;
    public static final int AACObjectSSR = 3;
    public static final int AACObjectScalable = 6;
    public static final int AACObjectXHE = 42;
    public static final int AV1Level2 = 1;
    public static final int AV1Level21 = 2;
    public static final int AV1Level22 = 4;
    public static final int AV1Level23 = 8;
    public static final int AV1Level3 = 16;
    public static final int AV1Level31 = 32;
    public static final int AV1Level32 = 64;
    public static final int AV1Level33 = 128;
    public static final int AV1Level4 = 256;
    public static final int AV1Level41 = 512;
    public static final int AV1Level42 = 1024;
    public static final int AV1Level43 = 2048;
    public static final int AV1Level5 = 4096;
    public static final int AV1Level51 = 8192;
    public static final int AV1Level52 = 16384;
    public static final int AV1Level53 = 32768;
    public static final int AV1Level6 = 65536;
    public static final int AV1Level61 = 131072;
    public static final int AV1Level62 = 262144;
    public static final int AV1Level63 = 524288;
    public static final int AV1Level7 = 1048576;
    public static final int AV1Level71 = 2097152;
    public static final int AV1Level72 = 4194304;
    public static final int AV1Level73 = 8388608;
    public static final int AV1ProfileMain10 = 2;
    public static final int AV1ProfileMain10HDR10 = 4096;
    public static final int AV1ProfileMain10HDR10Plus = 8192;
    public static final int AV1ProfileMain8 = 1;
    public static final int AVCLevel1 = 1;
    public static final int AVCLevel11 = 4;
    public static final int AVCLevel12 = 8;
    public static final int AVCLevel13 = 16;
    public static final int AVCLevel1b = 2;
    public static final int AVCLevel2 = 32;
    public static final int AVCLevel21 = 64;
    public static final int AVCLevel22 = 128;
    public static final int AVCLevel3 = 256;
    public static final int AVCLevel31 = 512;
    public static final int AVCLevel32 = 1024;
    public static final int AVCLevel4 = 2048;
    public static final int AVCLevel41 = 4096;
    public static final int AVCLevel42 = 8192;
    public static final int AVCLevel5 = 16384;
    public static final int AVCLevel51 = 32768;
    public static final int AVCLevel52 = 65536;
    public static final int AVCLevel6 = 131072;
    public static final int AVCLevel61 = 262144;
    public static final int AVCLevel62 = 524288;
    public static final int AVCProfileBaseline = 1;
    public static final int AVCProfileConstrainedBaseline = 65536;
    public static final int AVCProfileConstrainedHigh = 524288;
    public static final int AVCProfileExtended = 4;
    public static final int AVCProfileHigh = 8;
    public static final int AVCProfileHigh10 = 16;
    public static final int AVCProfileHigh422 = 32;
    public static final int AVCProfileHigh444 = 64;
    public static final int AVCProfileMain = 2;
    public static final int DolbyVisionLevelFhd24 = 4;
    public static final int DolbyVisionLevelFhd30 = 8;
    public static final int DolbyVisionLevelFhd60 = 16;
    public static final int DolbyVisionLevelHd24 = 1;
    public static final int DolbyVisionLevelHd30 = 2;
    public static final int DolbyVisionLevelUhd24 = 32;
    public static final int DolbyVisionLevelUhd30 = 64;
    public static final int DolbyVisionLevelUhd48 = 128;
    public static final int DolbyVisionLevelUhd60 = 256;
    public static final int DolbyVisionProfileDvav110 = 1024;
    public static final int DolbyVisionProfileDvavPen = 2;
    public static final int DolbyVisionProfileDvavPer = 1;
    public static final int DolbyVisionProfileDvavSe = 512;
    public static final int DolbyVisionProfileDvheDen = 8;
    public static final int DolbyVisionProfileDvheDer = 4;
    public static final int DolbyVisionProfileDvheDtb = 128;
    public static final int DolbyVisionProfileDvheDth = 64;
    public static final int DolbyVisionProfileDvheDtr = 16;
    public static final int DolbyVisionProfileDvheSt = 256;
    public static final int DolbyVisionProfileDvheStn = 32;
    public static final int H263Level10 = 1;
    public static final int H263Level20 = 2;
    public static final int H263Level30 = 4;
    public static final int H263Level40 = 8;
    public static final int H263Level45 = 16;
    public static final int H263Level50 = 32;
    public static final int H263Level60 = 64;
    public static final int H263Level70 = 128;
    public static final int H263ProfileBackwardCompatible = 4;
    public static final int H263ProfileBaseline = 1;
    public static final int H263ProfileH320Coding = 2;
    public static final int H263ProfileHighCompression = 32;
    public static final int H263ProfileHighLatency = 256;
    public static final int H263ProfileISWV2 = 8;
    public static final int H263ProfileISWV3 = 16;
    public static final int H263ProfileInterlace = 128;
    public static final int H263ProfileInternet = 64;
    public static final int HEVCHighTierLevel1 = 2;
    public static final int HEVCHighTierLevel2 = 8;
    public static final int HEVCHighTierLevel21 = 32;
    public static final int HEVCHighTierLevel3 = 128;
    public static final int HEVCHighTierLevel31 = 512;
    public static final int HEVCHighTierLevel4 = 2048;
    public static final int HEVCHighTierLevel41 = 8192;
    public static final int HEVCHighTierLevel5 = 32768;
    public static final int HEVCHighTierLevel51 = 131072;
    public static final int HEVCHighTierLevel52 = 524288;
    public static final int HEVCHighTierLevel6 = 2097152;
    public static final int HEVCHighTierLevel61 = 8388608;
    public static final int HEVCHighTierLevel62 = 33554432;
    public static final int HEVCMainTierLevel1 = 1;
    public static final int HEVCMainTierLevel2 = 4;
    public static final int HEVCMainTierLevel21 = 16;
    public static final int HEVCMainTierLevel3 = 64;
    public static final int HEVCMainTierLevel31 = 256;
    public static final int HEVCMainTierLevel4 = 1024;
    public static final int HEVCMainTierLevel41 = 4096;
    public static final int HEVCMainTierLevel5 = 16384;
    public static final int HEVCMainTierLevel51 = 65536;
    public static final int HEVCMainTierLevel52 = 262144;
    public static final int HEVCMainTierLevel6 = 1048576;
    public static final int HEVCMainTierLevel61 = 4194304;
    public static final int HEVCMainTierLevel62 = 16777216;
    public static final int HEVCProfileMain = 1;
    public static final int HEVCProfileMain10 = 2;
    public static final int HEVCProfileMain10HDR10 = 4096;
    public static final int HEVCProfileMain10HDR10Plus = 8192;
    public static final int HEVCProfileMainStill = 4;
    public static final int MPEG2LevelH14 = 2;
    public static final int MPEG2LevelHL = 3;
    public static final int MPEG2LevelHP = 4;
    public static final int MPEG2LevelLL = 0;
    public static final int MPEG2LevelML = 1;
    public static final int MPEG2Profile422 = 2;
    public static final int MPEG2ProfileHigh = 5;
    public static final int MPEG2ProfileMain = 1;
    public static final int MPEG2ProfileSNR = 3;
    public static final int MPEG2ProfileSimple = 0;
    public static final int MPEG2ProfileSpatial = 4;
    public static final int MPEG4Level0 = 1;
    public static final int MPEG4Level0b = 2;
    public static final int MPEG4Level1 = 4;
    public static final int MPEG4Level2 = 8;
    public static final int MPEG4Level3 = 16;
    public static final int MPEG4Level3b = 24;
    public static final int MPEG4Level4 = 32;
    public static final int MPEG4Level4a = 64;
    public static final int MPEG4Level5 = 128;
    public static final int MPEG4Level6 = 256;
    public static final int MPEG4ProfileAdvancedCoding = 4096;
    public static final int MPEG4ProfileAdvancedCore = 8192;
    public static final int MPEG4ProfileAdvancedRealTime = 1024;
    public static final int MPEG4ProfileAdvancedScalable = 16384;
    public static final int MPEG4ProfileAdvancedSimple = 32768;
    public static final int MPEG4ProfileBasicAnimated = 256;
    public static final int MPEG4ProfileCore = 4;
    public static final int MPEG4ProfileCoreScalable = 2048;
    public static final int MPEG4ProfileHybrid = 512;
    public static final int MPEG4ProfileMain = 8;
    public static final int MPEG4ProfileNbit = 16;
    public static final int MPEG4ProfileScalableTexture = 32;
    public static final int MPEG4ProfileSimple = 1;
    public static final int MPEG4ProfileSimpleFBA = 128;
    public static final int MPEG4ProfileSimpleFace = 64;
    public static final int MPEG4ProfileSimpleScalable = 2;
    public static final int VP8Level_Version0 = 1;
    public static final int VP8Level_Version1 = 2;
    public static final int VP8Level_Version2 = 4;
    public static final int VP8Level_Version3 = 8;
    public static final int VP8ProfileMain = 1;
    public static final int VP9Level1 = 1;
    public static final int VP9Level11 = 2;
    public static final int VP9Level2 = 4;
    public static final int VP9Level21 = 8;
    public static final int VP9Level3 = 16;
    public static final int VP9Level31 = 32;
    public static final int VP9Level4 = 64;
    public static final int VP9Level41 = 128;
    public static final int VP9Level5 = 256;
    public static final int VP9Level51 = 512;
    public static final int VP9Level52 = 1024;
    public static final int VP9Level6 = 2048;
    public static final int VP9Level61 = 4096;
    public static final int VP9Level62 = 8192;
    public static final int VP9Profile0 = 1;
    public static final int VP9Profile1 = 2;
    public static final int VP9Profile2 = 4;
    public static final int VP9Profile2HDR = 4096;
    public static final int VP9Profile2HDR10Plus = 16384;
    public static final int VP9Profile3 = 8;
    public static final int VP9Profile3HDR = 8192;
    public static final int VP9Profile3HDR10Plus = 32768;
    public int profile, level;
    public CodecProfileLevel() {}
  }
}
