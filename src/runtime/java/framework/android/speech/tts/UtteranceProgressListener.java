package android.speech.tts;

/** AOSP android.speech.tts.UtteranceProgressListener; see TextToSpeech, which never speaks. */
public abstract class UtteranceProgressListener {
  public abstract void onStart(String utteranceId);
  public abstract void onDone(String utteranceId);
  @Deprecated public abstract void onError(String utteranceId);
  public void onError(String utteranceId, int errorCode) { onError(utteranceId); }
  public void onStop(String utteranceId, boolean interrupted) {}
  public void onBeginSynthesis(String utteranceId, int sampleRateInHz, int audioFormat, int channelCount) {}
  public void onAudioAvailable(String utteranceId, byte[] audio) {}
  public void onRangeStart(String utteranceId, int start, int end, int frame) {}
}
