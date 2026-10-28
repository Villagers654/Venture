package android.animation;

import java.util.ArrayList;

/** Listener and timing contract for animators; concrete frame producers supply execution. */
public abstract class Animator implements Cloneable {
  public static final long DURATION_INFINITE = -1;
  // AOSP 14 Animator.sBackgroundPauseDelay (AnimationHandler background pausing).
  private static long backgroundPauseDelay = 1000;
  public static long getBackgroundPauseDelay() { return backgroundPauseDelay; }
  public static void setBackgroundPauseDelay(long value) { backgroundPauseDelay = value; }
  private ArrayList<AnimatorListener> listeners = new ArrayList<>();
  private ArrayList<AnimatorPauseListener> pauseListeners = new ArrayList<>();
  private boolean paused;
  public interface AnimatorListener {
    void onAnimationStart(Animator animation);
    void onAnimationEnd(Animator animation);
    void onAnimationCancel(Animator animation);
    void onAnimationRepeat(Animator animation);
    default void onAnimationStart(Animator animation, boolean reverse) { onAnimationStart(animation); }
    default void onAnimationEnd(Animator animation, boolean reverse) { onAnimationEnd(animation); }
  }
  public interface AnimatorPauseListener {
    void onAnimationPause(Animator animation);
    void onAnimationResume(Animator animation);
  }
  public void addListener(AnimatorListener listener) { listeners.add(listener); }
  public void removeListener(AnimatorListener listener) { listeners.remove(listener); }
  public ArrayList<AnimatorListener> getListeners() { return listeners.isEmpty() ? null : listeners; }
  public void removeAllListeners() { listeners.clear(); pauseListeners.clear(); }
  public void addPauseListener(AnimatorPauseListener listener) { pauseListeners.add(listener); }
  public void removePauseListener(AnimatorPauseListener listener) { pauseListeners.remove(listener); }
  public abstract void start();
  public abstract void cancel();
  public abstract void end();
  public abstract long getStartDelay();
  public abstract void setStartDelay(long delay);
  public abstract Animator setDuration(long duration);
  public abstract long getDuration();
  public long getTotalDuration() {
    long duration = getDuration();
    return duration == DURATION_INFINITE ? DURATION_INFINITE : getStartDelay() + duration;
  }
  public abstract void setInterpolator(TimeInterpolator value);
  public TimeInterpolator getInterpolator() { return null; }
  public abstract boolean isRunning();
  public boolean isStarted() { return isRunning(); }
  public boolean isPaused() { return paused; }
  public void pause() {
    if (!isStarted() || paused) return;
    paused = true;
    for (var listener : new ArrayList<>(pauseListeners)) listener.onAnimationPause(this);
  }
  public void resume() {
    if (!paused) return;
    paused = false;
    for (var listener : new ArrayList<>(pauseListeners)) listener.onAnimationResume(this);
  }
  public Animator clone() {
    try {
      Animator copy = (Animator)super.clone();
      copy.listeners = new ArrayList<>(listeners);
      copy.pauseListeners = new ArrayList<>(pauseListeners);
      copy.paused = false;
      return copy;
    } catch (CloneNotSupportedException e) { throw new AssertionError(e); }
  }
}
