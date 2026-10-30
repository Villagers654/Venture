package android.media;

/** AOSP android.media.AudioTimestamp: a frame position and the CLOCK_MONOTONIC time it was presented. */
public final class AudioTimestamp {
  public static final int TIMEBASE_MONOTONIC = 0, TIMEBASE_BOOTTIME = 1;
  public long framePosition;
  public long nanoTime;
  public AudioTimestamp() {}
}
