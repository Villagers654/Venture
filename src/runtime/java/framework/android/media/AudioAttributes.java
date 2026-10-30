package android.media;

import android.os.Parcel;
import android.os.Parcelable;
import java.util.Objects;

/** AOSP android.media.AudioAttributes: what a sound is, for routing and focus. */
public final class AudioAttributes implements Parcelable {
  public static final int CONTENT_TYPE_UNKNOWN = 0, CONTENT_TYPE_SPEECH = 1, CONTENT_TYPE_MUSIC = 2,
      CONTENT_TYPE_MOVIE = 3, CONTENT_TYPE_SONIFICATION = 4;
  public static final int USAGE_UNKNOWN = 0, USAGE_MEDIA = 1, USAGE_VOICE_COMMUNICATION = 2,
      USAGE_VOICE_COMMUNICATION_SIGNALLING = 3, USAGE_ALARM = 4, USAGE_NOTIFICATION = 5,
      USAGE_NOTIFICATION_RINGTONE = 6, USAGE_NOTIFICATION_COMMUNICATION_REQUEST = 7,
      USAGE_NOTIFICATION_COMMUNICATION_INSTANT = 8, USAGE_NOTIFICATION_COMMUNICATION_DELAYED = 9,
      USAGE_NOTIFICATION_EVENT = 10, USAGE_ASSISTANCE_ACCESSIBILITY = 11,
      USAGE_ASSISTANCE_NAVIGATION_GUIDANCE = 12, USAGE_ASSISTANCE_SONIFICATION = 13, USAGE_GAME = 14,
      USAGE_ASSISTANT = 16;
  public static final int FLAG_AUDIBILITY_ENFORCED = 0x1, FLAG_HW_AV_SYNC = 0x10,
      FLAG_LOW_LATENCY = 0x100;
  public static final int ALLOW_CAPTURE_BY_ALL = 1, ALLOW_CAPTURE_BY_SYSTEM = 2, ALLOW_CAPTURE_BY_NONE = 3;
  public static final int SPATIALIZATION_BEHAVIOR_AUTO = 0, SPATIALIZATION_BEHAVIOR_NEVER = 1;

  private int usage = USAGE_UNKNOWN, contentType = CONTENT_TYPE_UNKNOWN, flags, capturePolicy = ALLOW_CAPTURE_BY_ALL;
  private int spatializationBehavior = SPATIALIZATION_BEHAVIOR_AUTO, legacyStream = -1;
  private boolean hapticChannelsMuted = true;

  private AudioAttributes() {}
  public int getUsage() { return usage; }
  public int getContentType() { return contentType; }
  public int getFlags() { return flags; }
  public int getAllowedCapturePolicy() { return capturePolicy; }
  public int getSpatializationBehavior() { return spatializationBehavior; }
  public boolean isContentSpatialized() { return false; }
  public boolean areHapticChannelsMuted() { return hapticChannelsMuted; }
  // AudioAttributes.toLegacyStreamType for the usages apps set.
  public int getVolumeControlStream() {
    if (legacyStream >= 0) return legacyStream;
    return switch (usage) {
      case USAGE_VOICE_COMMUNICATION -> AudioManager.STREAM_VOICE_CALL;
      case USAGE_ALARM -> AudioManager.STREAM_ALARM;
      case USAGE_NOTIFICATION_RINGTONE -> AudioManager.STREAM_RING;
      case USAGE_NOTIFICATION, USAGE_NOTIFICATION_COMMUNICATION_REQUEST, USAGE_NOTIFICATION_COMMUNICATION_INSTANT,
           USAGE_NOTIFICATION_COMMUNICATION_DELAYED, USAGE_NOTIFICATION_EVENT -> AudioManager.STREAM_NOTIFICATION;
      default -> AudioManager.STREAM_MUSIC;
    };
  }
  @Override public boolean equals(Object o) {
    return o instanceof AudioAttributes a && a.usage == usage && a.contentType == contentType && a.flags == flags
        && a.capturePolicy == capturePolicy && a.spatializationBehavior == spatializationBehavior
        && a.legacyStream == legacyStream && a.hapticChannelsMuted == hapticChannelsMuted;
  }
  @Override public int hashCode() { return Objects.hash(usage, contentType, flags, capturePolicy, legacyStream); }
  @Override public String toString() {
    return "AudioAttributes: usage=" + usage + " content=" + contentType + " flags=0x" + Integer.toHexString(flags);
  }
  public int describeContents() { return 0; }
  public void writeToParcel(Parcel dest, int f) { dest.writeParcelable(this, f); }
  public static final Creator<AudioAttributes> CREATOR = new Creator<>() {
    public AudioAttributes createFromParcel(Parcel p) { return p.readParcelable(null); }
    public AudioAttributes[] newArray(int size) { return new AudioAttributes[size]; }
  };

  public static class Builder {
    private final AudioAttributes value = new AudioAttributes();
    public Builder() {}
    public Builder(AudioAttributes attributes) {
      value.usage = attributes.usage; value.contentType = attributes.contentType; value.flags = attributes.flags;
      value.capturePolicy = attributes.capturePolicy; value.spatializationBehavior = attributes.spatializationBehavior;
      value.legacyStream = attributes.legacyStream; value.hapticChannelsMuted = attributes.hapticChannelsMuted;
    }
    public AudioAttributes build() {
      AudioAttributes out = new AudioAttributes();
      out.usage = value.usage; out.contentType = value.contentType; out.flags = value.flags;
      out.capturePolicy = value.capturePolicy; out.spatializationBehavior = value.spatializationBehavior;
      out.legacyStream = value.legacyStream; out.hapticChannelsMuted = value.hapticChannelsMuted;
      return out;
    }
    public Builder setUsage(int usage) {
      value.usage = usage >= USAGE_UNKNOWN && usage <= USAGE_ASSISTANT && usage != 15 ? usage : USAGE_UNKNOWN;
      return this;
    }
    public Builder setContentType(int contentType) {
      value.contentType = contentType >= CONTENT_TYPE_UNKNOWN && contentType <= CONTENT_TYPE_SONIFICATION
          ? contentType : CONTENT_TYPE_UNKNOWN;
      return this;
    }
    public Builder setFlags(int flags) { value.flags |= flags & (FLAG_AUDIBILITY_ENFORCED | FLAG_HW_AV_SYNC | FLAG_LOW_LATENCY); return this; }
    public Builder setAllowedCapturePolicy(int policy) {
      if (policy < ALLOW_CAPTURE_BY_ALL || policy > ALLOW_CAPTURE_BY_NONE)
        throw new IllegalArgumentException("Unknown allow playback capture policy");
      value.capturePolicy = policy;
      return this;
    }
    public Builder setSpatializationBehavior(int behavior) { value.spatializationBehavior = behavior; return this; }
    public Builder setIsContentSpatialized(boolean spatialized) { return this; }
    public Builder setHapticChannelsMuted(boolean muted) { value.hapticChannelsMuted = muted; return this; }
    // Legacy stream types set the matching usage, as AOSP's Builder does.
    public Builder setLegacyStreamType(int streamType) {
      value.legacyStream = streamType;
      value.usage = switch (streamType) {
        case AudioManager.STREAM_VOICE_CALL -> USAGE_VOICE_COMMUNICATION;
        case AudioManager.STREAM_ALARM -> USAGE_ALARM;
        case AudioManager.STREAM_RING -> USAGE_NOTIFICATION_RINGTONE;
        case AudioManager.STREAM_NOTIFICATION -> USAGE_NOTIFICATION;
        case AudioManager.STREAM_SYSTEM -> USAGE_ASSISTANCE_SONIFICATION;
        default -> USAGE_MEDIA;
      };
      value.contentType = streamType == AudioManager.STREAM_VOICE_CALL ? CONTENT_TYPE_SPEECH
          : streamType == AudioManager.STREAM_MUSIC ? CONTENT_TYPE_MUSIC : CONTENT_TYPE_SONIFICATION;
      return this;
    }
  }
}
