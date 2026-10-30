package android.media;
import java.nio.ByteBuffer;
import java.util.HashMap;
import java.util.Map;
/** Typed media-format metadata. It does not advertise or instantiate a codec. */
public final class MediaFormat {
  public static final String KEY_MIME = "mime", KEY_WIDTH = "width", KEY_HEIGHT = "height",
      KEY_SAMPLE_RATE = "sample-rate", KEY_CHANNEL_COUNT = "channel-count", KEY_BIT_RATE = "bitrate",
      KEY_FRAME_RATE = "frame-rate", KEY_DURATION = "durationUs", KEY_MAX_INPUT_SIZE = "max-input-size";
  private final Map<String, Object> values = new HashMap<>();
  public static MediaFormat createVideoFormat(String mime, int width, int height) {
    MediaFormat value = new MediaFormat(); value.setString(KEY_MIME, mime);
    value.setInteger(KEY_WIDTH, width); value.setInteger(KEY_HEIGHT, height); return value;
  }
  public static MediaFormat createAudioFormat(String mime, int rate, int channels) {
    MediaFormat value = new MediaFormat(); value.setString(KEY_MIME, mime);
    value.setInteger(KEY_SAMPLE_RATE, rate); value.setInteger(KEY_CHANNEL_COUNT, channels); return value;
  }
  public boolean containsKey(String key) { return values.containsKey(key); }
  public void setInteger(String key, int value) { values.put(key, value); }
  public int getInteger(String key) { return (Integer) values.get(key); }
  public int getInteger(String key, int fallback) { return containsKey(key) ? getInteger(key) : fallback; }
  public void setLong(String key, long value) { values.put(key, value); }
  public long getLong(String key) { return (Long) values.get(key); }
  public void setFloat(String key, float value) { values.put(key, value); }
  public float getFloat(String key) { return (Float) values.get(key); }
  public float getFloat(String key, float fallback) { return containsKey(key) ? getFloat(key) : fallback; }
  public long getLong(String key, long fallback) { return containsKey(key) ? getLong(key) : fallback; }
  public String getString(String key, String fallback) { String value = getString(key); return value != null ? value : fallback; }
  public Number getNumber(String key) { return (Number) values.get(key); }
  public Number getNumber(String key, Number fallback) { Number value = getNumber(key); return value != null ? value : fallback; }
  public void removeKey(String key) { values.remove(key); }
  public java.util.Set<String> getKeys() { return values.keySet(); }
  // AOSP stores features as "feature-<name>" integer keys.
  public void setFeatureEnabled(String feature, boolean enabled) { setInteger("feature-" + feature, enabled ? 1 : 0); }
  public boolean getFeatureEnabled(String feature) {
    Object value = values.get("feature-" + feature);
    if (!(value instanceof Integer i)) throw new IllegalArgumentException("feature is not specified");
    return i != 0;
  }
  public void setString(String key, String value) { values.put(key, value); }
  public String getString(String key) { return (String) values.get(key); }
  public void setByteBuffer(String key, ByteBuffer value) { values.put(key, value); }
  public ByteBuffer getByteBuffer(String key) { return (ByteBuffer) values.get(key); }
  public String toString() { return values.toString(); }
}
