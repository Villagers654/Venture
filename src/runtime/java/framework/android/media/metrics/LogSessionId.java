package android.media.metrics;

/** AOSP android.media.metrics.LogSessionId (API 31). */
public final class LogSessionId {
  public static final LogSessionId LOG_SESSION_ID_NONE = new LogSessionId("");
  private final String id;
  public LogSessionId(String id) { this.id = java.util.Objects.requireNonNull(id); }
  public String getStringId() { return id; }
  @Override public String toString() { return id; }
  @Override public boolean equals(Object o) { return o instanceof LogSessionId other && id.equals(other.id); }
  @Override public int hashCode() { return id.hashCode(); }
}
