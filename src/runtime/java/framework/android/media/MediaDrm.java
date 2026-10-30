package android.media;

import java.util.UUID;

/** AOSP android.media.MediaDrm. Venture has no DRM plugins: no scheme is supported. */
public final class MediaDrm implements AutoCloseable {
  public static boolean isCryptoSchemeSupported(UUID uuid) { return false; }
  public static boolean isCryptoSchemeSupported(UUID uuid, String mimeType) { return false; }
  public static java.util.List<UUID> getSupportedCryptoSchemes() { return java.util.List.of(); }
  public MediaDrm(UUID uuid) throws UnsupportedSchemeException {
    throw new UnsupportedSchemeException("Failed to instantiate drm object.");
  }
  @Override public void close() {}
  public void release() {}
  public static final class MediaDrmStateException extends IllegalStateException {
    private final int errorCode;
    public MediaDrmStateException(int errorCode, String detailMessage) { super(detailMessage); this.errorCode = errorCode; }
    public int getErrorCode() { return errorCode; }
    public String getDiagnosticInfo() { return "android.media.MediaDrm.error_" + (errorCode < 0 ? "neg_" : "") + Math.abs(errorCode); }
  }
}
