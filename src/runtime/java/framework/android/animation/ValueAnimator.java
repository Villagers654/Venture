package android.animation;

import android.os.Looper;
import android.util.AndroidRuntimeException;
import android.view.Choreographer;
import java.util.ArrayList;

/**
 * AOSP frameworks/base/core/java/android/animation/ValueAnimator.java: a timed
 * fraction driven by the starting Looper thread's Choreographer frames (the
 * per-thread AnimationHandler), mapped through an interpolator onto evenly
 * spaced float or int keyframes. A single value animates from 0, as in
 * KeyframeSet.ofFloat/ofInt.
 */
public class ValueAnimator extends Animator {
  public static final int RESTART = 1, REVERSE = 2, INFINITE = -1;
  private static long frameDelay = 10;
  private static final TimeInterpolator DEFAULT_INTERPOLATOR =  // AccelerateDecelerateInterpolator
      input -> (float) (Math.cos((input + 1) * Math.PI) / 2.0) + 0.5f;

  public interface AnimatorUpdateListener { void onAnimationUpdate(ValueAnimator animation); }

  private ArrayList<AnimatorUpdateListener> updateListeners = new ArrayList<>();
  private float[] floats;
  private int[] ints;
  private long duration = 300, startDelay;
  private int repeatCount, repeatMode = RESTART;
  private TimeInterpolator interpolator = DEFAULT_INTERPOLATOR;
  private Object value;
  private float fraction;
  private long playTime;
  // Frame-clock state: startTime is the first frame's time minus the play time
  // already consumed; -1 until the first frame after start or resume.
  private long startTime = -1, delayStart = -1;
  private boolean started, running, startNotified;
  private float overallFraction;  // iterations played, clamped to [0, repeatCount + 1]
  private Choreographer choreographer;
  private AnimationHandler handler;  // the starting thread's
  private Choreographer.FrameCallback frame = this::doFrame;

  public ValueAnimator() {}
  public static ValueAnimator ofFloat(float... values) {
    var a = new ValueAnimator();
    a.setFloatValues(values);
    return a;
  }
  public static ValueAnimator ofInt(int... values) {
    var a = new ValueAnimator();
    a.setIntValues(values);
    return a;
  }
  public void setFloatValues(float... values) {
    if (values == null || values.length == 0) return;
    floats = values.length == 1 ? new float[] {0, values[0]} : values.clone();
    ints = null;
  }
  public void setIntValues(int... values) {
    if (values == null || values.length == 0) return;
    ints = values.length == 1 ? new int[] {0, values[0]} : values.clone();
    floats = null;
  }
  public static boolean areAnimatorsEnabled() { return true; }  // duration scale 1
  public static long getFrameDelay() { return frameDelay; }
  public static void setFrameDelay(long delay) { frameDelay = delay; }

  @Override public ValueAnimator setDuration(long value) {
    if (value < 0) throw new IllegalArgumentException("Animators cannot have negative duration: " + value);
    duration = value;
    return this;
  }
  @Override public long getDuration() { return duration; }
  @Override public long getTotalDuration() {
    return repeatCount == INFINITE ? DURATION_INFINITE : startDelay + duration * (repeatCount + 1);
  }
  @Override public long getStartDelay() { return startDelay; }
  @Override public void setStartDelay(long value) { startDelay = Math.max(0, value); }
  public void setRepeatCount(int value) { repeatCount = value; }
  public int getRepeatCount() { return repeatCount; }
  public void setRepeatMode(int value) { repeatMode = value; }
  public int getRepeatMode() { return repeatMode; }
  @Override public void setInterpolator(TimeInterpolator value) {
    interpolator = value != null ? value : input -> input;  // null means linear
  }
  @Override public TimeInterpolator getInterpolator() { return interpolator; }
  public Object getAnimatedValue() { return value; }
  public float getAnimatedFraction() { return fraction; }
  public long getCurrentPlayTime() { return playTime; }
  public void addUpdateListener(AnimatorUpdateListener listener) { updateListeners.add(listener); }
  public void removeUpdateListener(AnimatorUpdateListener listener) { updateListeners.remove(listener); }
  public void removeAllUpdateListeners() { updateListeners.clear(); }
  @Override public boolean isRunning() { return running; }
  @Override public boolean isStarted() { return started; }

  private static void requireLooper() {
    if (Looper.myLooper() == null)
      throw new AndroidRuntimeException("Animators may only be run on Looper threads");
  }
  @Override public void start() {
    requireLooper();
    if (started) cancelFrames();
    started = true;
    running = false;
    startNotified = false;
    overallFraction = 0;
    playTime = 0;
    startTime = -1;
    choreographer = Choreographer.getInstance();
    if (handler != null) handler.remove(this);
    handler = AnimationHandler.getInstance();
    handler.add(this);
    if (startDelay == 0) {
      beginRunning();
      animateValue(0);
    } else {
      delayStart = -1;
    }
    choreographer.postFrameCallback(frame);
  }
  private void beginRunning() {
    running = true;
    if (startNotified) return;
    startNotified = true;
    var listeners = getListeners();
    if (listeners != null)
      for (var l : new ArrayList<>(listeners)) l.onAnimationStart(this, false);
  }
  @Override public void end() {
    requireLooper();
    if (!running) {
      started = true;
      beginRunning();
    }
    boolean backward = repeatMode == REVERSE && repeatCount > 0 && repeatCount % 2 == 1;
    animateValue(backward ? 0 : 1);
    finish();
  }
  @Override public void cancel() {
    requireLooper();
    if (!started && !running) return;
    if (!startNotified) beginRunning();
    var listeners = getListeners();
    if (listeners != null)
      for (var l : new ArrayList<>(listeners)) l.onAnimationCancel(this);
    finish();
  }
  @Override public void pause() {
    boolean wasPaused = isPaused();
    super.pause();
    if (!wasPaused && isPaused()) startTime = -1;  // play time is kept; resume restarts the clock
  }
  @Override public void resume() {
    boolean wasPaused = isPaused();
    super.resume();
    // AOSP addAnimationCallback registers the animator with its AnimationHandler at
    // most once; a paused start delay resumes too.
    if (wasPaused && !isPaused() && started && choreographer != null) {
      choreographer.removeFrameCallback(frame);
      choreographer.postFrameCallback(frame);
    }
  }
  private void cancelFrames() {
    if (choreographer != null) choreographer.removeFrameCallback(frame);
  }
  private void finish() {
    cancelFrames();
    if (handler != null) handler.remove(this);
    boolean notify = started || running;
    started = running = false;
    startNotified = false;
    if (!notify) return;
    var listeners = getListeners();
    if (listeners != null)
      for (var l : new ArrayList<>(listeners)) l.onAnimationEnd(this, false);
  }
  private void doFrame(long frameTimeNanos) {
    if (!started || isPaused()) return;
    long now = frameTimeNanos / 1_000_000;
    if (!running) {
      if (delayStart < 0) delayStart = now;
      if (now - delayStart < startDelay) {
        choreographer.postFrameCallback(frame);
        return;
      }
      beginRunning();
      startTime = now;
    }
    if (startTime < 0) startTime = now - playTime;
    playTime = now - startTime;
    // AOSP animateBasedOnTime: one onAnimationRepeat for a frame that enters a new
    // iteration, done once the overall fraction reaches repeatCount + 1; a zero
    // duration skips to the end regardless of the repeat count.
    float overall = duration > 0 ? (float) playTime / duration : 1f;
    boolean lastIterationFinished = repeatCount != INFINITE && overall >= repeatCount + 1;
    boolean done = duration == 0 || lastIterationFinished;
    if (!done && (int) overall > (int) overallFraction) {
      var listeners = getListeners();
      if (listeners != null)
        for (var l : new ArrayList<>(listeners)) l.onAnimationRepeat(this);
    }
    overallFraction = clampFraction(overall);
    animateValue(iterationFraction(overallFraction));
    if (done) finish();
    else if (started) choreographer.postFrameCallback(frame);
  }
  private float clampFraction(float fraction) {
    fraction = Math.max(0f, fraction);
    return repeatCount == INFINITE ? fraction : Math.min(fraction, repeatCount + 1);
  }
  // AOSP getCurrentIterationFraction: a positive whole overall fraction completes the
  // previous iteration (fraction 1) rather than starting the next one at 0.
  private float iterationFraction(float overall) {
    int iteration = (int) Math.floor(overall);
    if (overall == iteration && overall > 0) --iteration;
    float current = overall - iteration;
    return repeatMode == REVERSE && iteration % 2 == 1 ? 1f - current : current;
  }
  void animateValue(float linear) {
    fraction = interpolator.getInterpolation(linear);
    value = valueAt(fraction);
    for (var l : new ArrayList<>(updateListeners)) l.onAnimationUpdate(this);
  }
  private Object valueAt(float f) {
    int count = floats != null ? floats.length : ints != null ? ints.length : 0;
    if (count == 0) return null;
    // Evenly spaced keyframes; fractions outside [0, 1] extrapolate the end intervals.
    float scaled = f * (count - 1);
    int i = Math.max(0, Math.min(count - 2, (int) Math.floor(scaled)));
    float local = scaled - i;
    if (floats != null) return floats[i] + local * (floats[i + 1] - floats[i]);
    return ints[i] + (int) (local * (ints[i + 1] - ints[i]));
  }
  @Override public ValueAnimator clone() {
    var copy = (ValueAnimator) super.clone();
    copy.updateListeners = new ArrayList<>(updateListeners);
    copy.floats = floats == null ? null : floats.clone();
    copy.ints = ints == null ? null : ints.clone();
    copy.started = copy.running = copy.startNotified = false;
    copy.startTime = copy.delayStart = -1;
    copy.choreographer = null;
    copy.handler = null;
    copy.frame = copy::doFrame;
    return copy;
  }
}
