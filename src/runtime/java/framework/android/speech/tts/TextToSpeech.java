package android.speech.tts;

import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * AOSP android.speech.tts.TextToSpeech on a device with no speech engine installed. Like AOSP
 * when no engine binds, the init listener hears ERROR on the main thread and every request
 * fails. Unreal's GameActivity implements OnInitListener, so its class cannot load without
 * this one (Espire 2).
 */
public class TextToSpeech {
  public static final int SUCCESS = 0, ERROR = -1, STOPPED = -2, ERROR_SYNTHESIS = -3, ERROR_SERVICE = -4,
      ERROR_OUTPUT = -5, ERROR_NETWORK = -6, ERROR_NETWORK_TIMEOUT = -7, ERROR_INVALID_REQUEST = -8,
      ERROR_NOT_INSTALLED_YET = -9;
  public static final int QUEUE_FLUSH = 0, QUEUE_ADD = 1;
  public static final int LANG_COUNTRY_VAR_AVAILABLE = 2, LANG_COUNTRY_AVAILABLE = 1, LANG_AVAILABLE = 0,
      LANG_MISSING_DATA = -1, LANG_NOT_SUPPORTED = -2;
  public static final String ACTION_TTS_QUEUE_PROCESSING_COMPLETED =
      "android.speech.tts.TTS_QUEUE_PROCESSING_COMPLETED";

  public interface OnInitListener { void onInit(int status); }
  @Deprecated public interface OnUtteranceCompletedListener { void onUtteranceCompleted(String utteranceId); }

  public static class EngineInfo {
    public String name, label;
    public int icon;
    public EngineInfo() {}
  }

  public class Engine {
    public static final int DEFAULT_RATE = 100, DEFAULT_PITCH = 100;
    public static final int DEFAULT_STREAM = 3;
    public static final int CHECK_VOICE_DATA_PASS = 1, CHECK_VOICE_DATA_FAIL = 0;
    public static final String ACTION_CHECK_TTS_DATA = "android.speech.tts.engine.CHECK_TTS_DATA",
        ACTION_INSTALL_TTS_DATA = "android.speech.tts.engine.INSTALL_TTS_DATA",
        ACTION_GET_SAMPLE_TEXT = "android.speech.tts.engine.GET_SAMPLE_TEXT",
        INTENT_ACTION_TTS_SERVICE = "android.intent.action.TTS_SERVICE",
        EXTRA_AVAILABLE_VOICES = "availableVoices", EXTRA_UNAVAILABLE_VOICES = "unavailableVoices",
        KEY_PARAM_STREAM = "streamType", KEY_PARAM_UTTERANCE_ID = "utteranceId", KEY_PARAM_VOLUME = "volume",
        KEY_PARAM_PAN = "pan", KEY_PARAM_SESSION_ID = "sessionId",
        KEY_FEATURE_NETWORK_SYNTHESIS = "networkTts", KEY_FEATURE_EMBEDDED_SYNTHESIS = "embeddedTts";
  }

  public TextToSpeech(Context context, OnInitListener listener) { this(context, listener, null); }
  public TextToSpeech(Context context, OnInitListener listener, String engine) {
    if (listener == null) return;
    // Delivered after the constructor returns, as AOSP does once the bind fails, so a listener
    // reading its own TextToSpeech field sees it set.
    Looper main = Looper.getMainLooper();
    if (main != null) new Handler(main).post(() -> listener.onInit(ERROR));
    else listener.onInit(ERROR);
  }

  public static int getMaxSpeechInputLength() { return 4000; }

  public int speak(CharSequence text, int queueMode, Bundle params, String utteranceId) { return ERROR; }
  @Deprecated public int speak(String text, int queueMode, HashMap<String, String> params) { return ERROR; }
  public int playSilentUtterance(long durationInMs, int queueMode, String utteranceId) { return ERROR; }
  @Deprecated public int playSilence(long durationInMs, int queueMode, HashMap<String, String> params) {
    return ERROR;
  }
  public int playEarcon(String earcon, int queueMode, Bundle params, String utteranceId) { return ERROR; }
  public int synthesizeToFile(CharSequence text, Bundle params, java.io.File file, String utteranceId) {
    return ERROR;
  }
  public int addSpeech(CharSequence text, String packageName, int resourceId) { return ERROR; }
  public int addSpeech(String text, String filename) { return ERROR; }
  public int addEarcon(String earcon, String packageName, int resourceId) { return ERROR; }
  public boolean isSpeaking() { return false; }
  public int stop() { return ERROR; }
  public void shutdown() {}
  public int setSpeechRate(float speechRate) { return ERROR; }
  public int setPitch(float pitch) { return ERROR; }
  public int setLanguage(Locale locale) { return LANG_NOT_SUPPORTED; }
  public int isLanguageAvailable(Locale locale) { return LANG_NOT_SUPPORTED; }
  @Deprecated public Locale getLanguage() { return null; }
  public Locale getDefaultLanguage() { return null; }
  public Set<Locale> getAvailableLanguages() { return null; }
  public String getDefaultEngine() { return null; }
  public List<EngineInfo> getEngines() { return Collections.emptyList(); }
  @Deprecated public boolean areDefaultsEnforced() { return false; }
  public int setEngineByPackageName(String enginePackageName) { return ERROR; }
  public int setOnUtteranceProgressListener(UtteranceProgressListener listener) { return SUCCESS; }
  @Deprecated public int setOnUtteranceCompletedListener(OnUtteranceCompletedListener listener) { return SUCCESS; }
}
