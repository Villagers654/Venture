package android.media;

import java.util.HashMap;
import java.util.List;
import java.util.UUID;

/**
 * AOSP android.media.MediaDrm. Venture has no DRM plugins, so it answers like a
 * device without the scheme: isCryptoSchemeSupported is false and the
 * constructor throws UnsupportedSchemeException (AOSP MediaDrm(UUID) on a
 * failed native setup). Players such as ExoPlayer then use clear playback.
 * The listener and value types exist because players reference them while
 * building their DRM session managers, before asking for a scheme.
 */
public final class MediaDrm implements AutoCloseable {
  public static final int EVENT_PROVISION_REQUIRED = 1, EVENT_KEY_REQUIRED = 2,
      EVENT_KEY_EXPIRED = 3, EVENT_VENDOR_DEFINED = 4, EVENT_SESSION_RECLAIMED = 5;
  public static final int KEY_TYPE_STREAMING = 1, KEY_TYPE_OFFLINE = 2, KEY_TYPE_RELEASE = 3;
  public static final int HDCP_LEVEL_UNKNOWN = 0, HDCP_NONE = 1, HDCP_V1 = 2, HDCP_V2 = 3,
      HDCP_V2_1 = 4, HDCP_V2_2 = 5, HDCP_V2_3 = 6, HDCP_NO_DIGITAL_OUTPUT = Integer.MAX_VALUE;
  public static final int SECURITY_LEVEL_UNKNOWN = 0, SECURITY_LEVEL_SW_SECURE_CRYPTO = 1,
      SECURITY_LEVEL_SW_SECURE_DECODE = 2, SECURITY_LEVEL_HW_SECURE_CRYPTO = 3,
      SECURITY_LEVEL_HW_SECURE_DECODE = 4, SECURITY_LEVEL_HW_SECURE_ALL = 5;
  public static final int OFFLINE_LICENSE_STATE_UNKNOWN = 0, OFFLINE_LICENSE_STATE_USABLE = 1,
      OFFLINE_LICENSE_STATE_RELEASED = 2;
  public static final String PROPERTY_VENDOR = "vendor", PROPERTY_VERSION = "version",
      PROPERTY_DESCRIPTION = "description", PROPERTY_ALGORITHMS = "algorithms",
      PROPERTY_DEVICE_UNIQUE_ID = "deviceUniqueId";

  public static boolean isCryptoSchemeSupported(UUID uuid) { return false; }
  public static boolean isCryptoSchemeSupported(UUID uuid, String mimeType) { return false; }
  public static boolean isCryptoSchemeSupported(UUID uuid, String mimeType, int securityLevel) {
    return false;
  }
  public static List<UUID> getSupportedCryptoSchemes() { return List.of(); }
  public static int getMaxSecurityLevel() { return SECURITY_LEVEL_UNKNOWN; }

  public MediaDrm(UUID uuid) throws UnsupportedSchemeException {
    if (uuid == null) throw new IllegalArgumentException("uuid cannot be null");
    throw new UnsupportedSchemeException("Failed to instantiate drm object.");
  }

  // No instance can exist; these keep AOSP's declared surface for linking.
  private static IllegalStateException released() {
    return new IllegalStateException("MediaDrm instance is not usable");
  }
  @Override public void close() {}
  public void release() {}
  public byte[] openSession() throws NotProvisionedException, ResourceBusyException { throw released(); }
  public byte[] openSession(int level) throws NotProvisionedException, ResourceBusyException { throw released(); }
  public void closeSession(byte[] sessionId) { throw released(); }
  public KeyRequest getKeyRequest(byte[] scope, byte[] init, String mimeType, int keyType,
      HashMap<String, String> optionalParameters) throws NotProvisionedException { throw released(); }
  public byte[] provideKeyResponse(byte[] scope, byte[] response)
      throws NotProvisionedException, DeniedByServerException { throw released(); }
  public void restoreKeys(byte[] sessionId, byte[] keySetId) { throw released(); }
  public void removeKeys(byte[] sessionId) { throw released(); }
  public HashMap<String, String> queryKeyStatus(byte[] sessionId) { throw released(); }
  public ProvisionRequest getProvisionRequest() { throw released(); }
  public void provideProvisionResponse(byte[] response) throws DeniedByServerException { throw released(); }
  public List<byte[]> getOfflineLicenseKeySetIds() { throw released(); }
  public void removeOfflineLicense(byte[] keySetId) { throw released(); }
  public int getOfflineLicenseState(byte[] keySetId) { throw released(); }
  public boolean requiresSecureDecoder(String mime) { throw released(); }
  public boolean requiresSecureDecoder(String mime, int level) { throw released(); }
  public String getPropertyString(String propertyName) { throw released(); }
  public byte[] getPropertyByteArray(String propertyName) { throw released(); }
  public void setPropertyString(String propertyName, String value) { throw released(); }
  public void setPropertyByteArray(String propertyName, byte[] value) { throw released(); }
  public int getConnectedHdcpLevel() { throw released(); }
  public int getMaxHdcpLevel() { throw released(); }
  public int getSecurityLevel(byte[] sessionId) { throw released(); }
  public android.os.PersistableBundle getMetrics() { throw released(); }
  public PlaybackComponent getPlaybackComponent(byte[] sessionId) { throw released(); }
  public void setOnEventListener(OnEventListener listener) { throw released(); }
  public void setOnEventListener(OnEventListener listener, android.os.Handler handler) { throw released(); }
  public void setOnExpirationUpdateListener(OnExpirationUpdateListener listener, android.os.Handler handler) {
    throw released();
  }
  public void clearOnExpirationUpdateListener() { throw released(); }
  public void setOnKeyStatusChangeListener(OnKeyStatusChangeListener listener, android.os.Handler handler) {
    throw released();
  }
  public void clearOnKeyStatusChangeListener() { throw released(); }

  public interface OnEventListener {
    void onEvent(MediaDrm md, byte[] sessionId, int event, int extra, byte[] data);
  }
  public interface OnExpirationUpdateListener {
    void onExpirationUpdate(MediaDrm md, byte[] sessionId, long expirationTime);
  }
  public interface OnKeyStatusChangeListener {
    void onKeyStatusChange(MediaDrm md, byte[] sessionId, List<KeyStatus> keyInformation,
        boolean hasNewUsableKey);
  }
  public interface OnSessionLostStateListener {
    void onSessionLostState(MediaDrm md, byte[] sessionId);
  }

  public static final class KeyStatus {
    public static final int STATUS_USABLE = 0, STATUS_EXPIRED = 1, STATUS_OUTPUT_NOT_ALLOWED = 2,
        STATUS_PENDING = 3, STATUS_INTERNAL_ERROR = 4, STATUS_USABLE_IN_FUTURE = 5;
    private final byte[] keyId;
    private final int statusCode;
    KeyStatus(byte[] keyId, int statusCode) { this.keyId = keyId; this.statusCode = statusCode; }
    public int getStatusCode() { return statusCode; }
    public byte[] getKeyId() { return keyId; }
  }
  public static final class KeyRequest {
    public static final int REQUEST_TYPE_INITIAL = 0, REQUEST_TYPE_RENEWAL = 1,
        REQUEST_TYPE_RELEASE = 2, REQUEST_TYPE_NONE = 3, REQUEST_TYPE_UPDATE = 4;
    private final byte[] data;
    private final String defaultUrl;
    private final int requestType;
    KeyRequest(byte[] data, String defaultUrl, int requestType) {
      this.data = data; this.defaultUrl = defaultUrl; this.requestType = requestType;
    }
    public byte[] getData() { return data; }
    public String getDefaultUrl() { return defaultUrl; }
    public int getRequestType() { return requestType; }
  }
  public static final class ProvisionRequest {
    private final byte[] data;
    private final String defaultUrl;
    ProvisionRequest(byte[] data, String defaultUrl) { this.data = data; this.defaultUrl = defaultUrl; }
    public byte[] getData() { return data; }
    public String getDefaultUrl() { return defaultUrl; }
  }
  public final class PlaybackComponent {
    private final byte[] sessionId;
    private android.media.metrics.LogSessionId logSessionId =
        android.media.metrics.LogSessionId.LOG_SESSION_ID_NONE;
    PlaybackComponent(byte[] sessionId) { this.sessionId = sessionId; }
    public void setLogSessionId(android.media.metrics.LogSessionId id) { logSessionId = id; }
    public android.media.metrics.LogSessionId getLogSessionId() { return logSessionId; }
  }
  public static final class MediaDrmStateException extends IllegalStateException {
    private final int errorCode;
    public MediaDrmStateException(int errorCode, String detailMessage) { super(detailMessage); this.errorCode = errorCode; }
    public int getErrorCode() { return errorCode; }
    public String getDiagnosticInfo() { return "android.media.MediaDrm.error_" + (errorCode < 0 ? "neg_" : "") + Math.abs(errorCode); }
  }
}
