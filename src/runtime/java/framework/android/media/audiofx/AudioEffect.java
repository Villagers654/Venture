package android.media.audiofx;

import java.util.UUID;

/**
 * Audio effects attach to a session. The host mixer applies none, so effects
 * keep their settings without changing the sound; capture pre-processing
 * (noise suppression, echo cancellation, gain control) reports unavailable,
 * as on devices without it.
 */
public class AudioEffect {
  public static final UUID EFFECT_TYPE_ENV_REVERB = UUID.fromString("c2e5d5f0-94bd-4763-9cac-4e234d06839e"),
      EFFECT_TYPE_PRESET_REVERB = UUID.fromString("47382d60-ddd8-11db-bf3a-0002a5d5c51b"),
      EFFECT_TYPE_EQUALIZER = UUID.fromString("0bed4300-ddd6-11db-8f34-0002a5d5c51b"),
      EFFECT_TYPE_BASS_BOOST = UUID.fromString("0634f220-ddd4-11db-a0fc-0002a5d5c51b"),
      EFFECT_TYPE_VIRTUALIZER = UUID.fromString("37cc2c00-dddd-11db-8577-0002a5d5c51b"),
      EFFECT_TYPE_AGC = UUID.fromString("0a8abfe0-654c-11e0-ba26-0002a5d5c51b"),
      EFFECT_TYPE_AEC = UUID.fromString("7b491460-8d4d-11e0-bd61-0002a5d5c51b"),
      EFFECT_TYPE_NS = UUID.fromString("58b4b260-8e06-11e0-aa8e-0002a5d5c51b"),
      EFFECT_TYPE_LOUDNESS_ENHANCER = UUID.fromString("fe3199be-aed0-413f-87bb-11260eb63cf1"),
      EFFECT_TYPE_DYNAMICS_PROCESSING = UUID.fromString("7261676f-6d75-7369-6364-28e2fd3ac39e");
  public static final UUID EFFECT_TYPE_NULL = UUID.fromString("ec7178ec-e5e1-4432-a3f4-4657e6795210");
  public static final int SUCCESS = 0, ERROR = -1, ALREADY_EXISTS = -2, ERROR_NO_INIT = -3, ERROR_BAD_VALUE = -4,
      ERROR_INVALID_OPERATION = -5, ERROR_NO_MEMORY = -6, ERROR_DEAD_OBJECT = -7, CONTENT_TYPE_MUSIC = 0,
      CONTENT_TYPE_MOVIE = 1, CONTENT_TYPE_GAME = 2, CONTENT_TYPE_VOICE = 3;
  public static final String EFFECT_INSERT = "Insert", EFFECT_AUXILIARY = "Auxiliary",
      EFFECT_PRE_PROCESSING = "Pre Processing", EFFECT_POST_PROCESSING = "Post Processing";
  public static final String ACTION_OPEN_AUDIO_EFFECT_CONTROL_SESSION =
      "android.media.action.OPEN_AUDIO_EFFECT_CONTROL_SESSION",
      ACTION_CLOSE_AUDIO_EFFECT_CONTROL_SESSION = "android.media.action.CLOSE_AUDIO_EFFECT_CONTROL_SESSION",
      ACTION_DISPLAY_AUDIO_EFFECT_CONTROL_PANEL = "android.media.action.DISPLAY_AUDIO_EFFECT_CONTROL_PANEL",
      EXTRA_AUDIO_SESSION = "android.media.extra.AUDIO_SESSION", EXTRA_PACKAGE_NAME = "android.media.extra.PACKAGE_NAME",
      EXTRA_CONTENT_TYPE = "android.media.extra.CONTENT_TYPE";

  public static class Descriptor {
    public UUID type, uuid;
    public String connectMode, name, implementor;
    public Descriptor() {}
    public Descriptor(String type, String uuid, String connectMode, String name, String implementor) {
      this.type = UUID.fromString(type);
      this.uuid = UUID.fromString(uuid);
      this.connectMode = connectMode;
      this.name = name;
      this.implementor = implementor;
    }
  }
  public interface OnEnableStatusChangeListener { void onEnableStatusChange(AudioEffect effect, boolean enabled); }
  public interface OnControlStatusChangeListener { void onControlStatusChange(AudioEffect effect, boolean controlGranted); }

  private static int nextId = 1;
  private final Descriptor descriptor;
  private final int id;
  private boolean enabled, released;

  AudioEffect(UUID type, String name, String mode) {
    descriptor = new Descriptor();
    descriptor.type = type;
    descriptor.uuid = type;
    descriptor.connectMode = mode;
    descriptor.name = name;
    descriptor.implementor = "Venture";
    synchronized (AudioEffect.class) { id = nextId++; }
  }
  /** The session's effects; the host mixer has none to list. */
  public static Descriptor[] queryEffects() { return new Descriptor[0]; }
  public static Descriptor[] queryPreProcessings(int audioSession) { return new Descriptor[0]; }
  public static boolean isEffectTypeAvailable(UUID type) { return false; }

  void checkState() {
    if (released) throw new IllegalStateException("AudioEffect was released");
  }
  public Descriptor getDescriptor() { checkState(); return descriptor; }
  public int getId() { checkState(); return id; }
  public int setEnabled(boolean value) { checkState(); enabled = value; return SUCCESS; }
  public boolean getEnabled() { checkState(); return enabled; }
  public boolean hasControl() { checkState(); return true; }
  public void release() { released = true; }
  public void setEnableStatusListener(OnEnableStatusChangeListener listener) {}
  public void setControlStatusListener(OnControlStatusChangeListener listener) {}
}
