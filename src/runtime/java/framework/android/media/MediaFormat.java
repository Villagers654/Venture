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
  public void setString(String key, String value) { values.put(key, value); }
  public String getString(String key) { return (String) values.get(key); }
  public void setByteBuffer(String key, ByteBuffer value) { values.put(key, value); }
  public ByteBuffer getByteBuffer(String key) { return (ByteBuffer) values.get(key); }
  public String toString() { return values.toString(); }
}
