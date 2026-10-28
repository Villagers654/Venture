package android.animation;

import android.view.Choreographer;
import java.util.ArrayList;
import java.util.IdentityHashMap;

/**
 * AOSP android-14.0.0_r1 frameworks/base/core/java/android/animation/AnimationHandler.java,
 * background pausing only: each Looper thread's visible windows request animators. When the
 * last request is withdrawn (the app's windows became invisible), infinite-duration animators
 * of that thread pause after Animator.getBackgroundPauseDelay(); a new request resumes them.
 */
public class AnimationHandler {
  private static final ThreadLocal<AnimationHandler> LOCAL = ThreadLocal.withInitial(AnimationHandler::new);
  private final ArrayList<ValueAnimator> animators = new ArrayList<>();
  private final IdentityHashMap<Object, Boolean> requestors = new IdentityHashMap<>();
  private final ArrayList<Animator> paused = new ArrayList<>();
  private final Choreographer.FrameCallback pauser = frameTimeNanos -> {
    if (!requestors.isEmpty()) return;
    for (var animator : new ArrayList<>(animators))
      if (animator.getTotalDuration() == Animator.DURATION_INFINITE && !animator.isPaused()) {
        paused.add(animator);
        animator.pause();
      }
  };

  static AnimationHandler getInstance() { return LOCAL.get(); }
  void add(ValueAnimator animator) { if (!animators.contains(animator)) animators.add(animator); }
  void remove(ValueAnimator animator) { animators.remove(animator); paused.remove(animator); }

  /** AOSP ViewRootImpl calls this with its app visibility; the caller's thread is affected. */
  public static void requestAnimatorsEnabled(boolean enable, Object requestor) {
    getInstance().requestAnimatorsEnabledImpl(enable, requestor);
  }
  private void requestAnimatorsEnabledImpl(boolean enable, Object requestor) {
    if (android.os.Looper.myLooper() == null) return;  // animators run only on Looper threads
    boolean wasEmpty = requestors.isEmpty();
    if (enable) requestors.put(requestor, Boolean.TRUE);
    else requestors.remove(requestor);
    boolean isEmpty = requestors.isEmpty();
    if (wasEmpty == isEmpty) return;
    var choreographer = Choreographer.getInstance();
    if (isEmpty) {
      choreographer.postFrameCallbackDelayed(pauser, Animator.getBackgroundPauseDelay());
    } else {
      choreographer.removeFrameCallback(pauser);
      for (var animator : new ArrayList<>(paused)) animator.resume();
      paused.clear();
    }
  }
}
