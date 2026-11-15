package android.transition;

import android.animation.TimeInterpolator;
import android.content.Context;
import android.util.AttributeSet;
import android.view.View;
import android.view.ViewGroup;
import java.util.ArrayList;

/** AOSP core/java/android/transition/TransitionSet.java. */
public class TransitionSet extends Transition {
  public static final int ORDERING_TOGETHER = 0, ORDERING_SEQUENTIAL = 1;
  private static final int FLAG_CHANGE_INTERPOLATOR = 0x01, FLAG_CHANGE_PROPAGATION = 0x02,
      FLAG_CHANGE_PATH_MOTION = 0x04, FLAG_CHANGE_EPICENTER = 0x08;
  ArrayList<Transition> transitions = new ArrayList<>();
  private boolean playTogether = true;
  private int changeFlags;
  private int currentListeners;
  private boolean started;

  public TransitionSet() {}
  public TransitionSet(Context context, AttributeSet attrs) { super(context, attrs); }

  public TransitionSet setOrdering(int ordering) {
    switch (ordering) {
      case ORDERING_SEQUENTIAL -> playTogether = false;
      case ORDERING_TOGETHER -> playTogether = true;
      default -> throw new IllegalArgumentException("Invalid parameter for TransitionSet ordering: " + ordering);
    }
    return this;
  }
  public int getOrdering() { return playTogether ? ORDERING_TOGETHER : ORDERING_SEQUENTIAL; }

  // AOSP addTransition: the set's duration, interpolator, propagation, path
  // motion and epicenter, where set, pass to the child.
  public TransitionSet addTransition(Transition transition) {
    if (transition != null) {
      transitions.add(transition);
      if (duration >= 0) transition.setDuration(duration);
      if ((changeFlags & FLAG_CHANGE_INTERPOLATOR) != 0) transition.setInterpolator(getInterpolator());
      if ((changeFlags & FLAG_CHANGE_PROPAGATION) != 0) transition.setPropagation(getPropagation());
      if ((changeFlags & FLAG_CHANGE_PATH_MOTION) != 0) transition.setPathMotion(getPathMotion());
      if ((changeFlags & FLAG_CHANGE_EPICENTER) != 0) transition.setEpicenterCallback(getEpicenterCallback());
    }
    return this;
  }
  public int getTransitionCount() { return transitions.size(); }
  public Transition getTransitionAt(int index) {
    return index < 0 || index >= transitions.size() ? null : transitions.get(index);
  }
  public TransitionSet removeTransition(Transition transition) {
    transitions.remove(transition);
    return this;
  }

  @Override public TransitionSet setDuration(long duration) {
    super.setDuration(duration);
    if (this.duration >= 0) for (Transition transition : transitions) transition.setDuration(duration);
    return this;
  }
  @Override public TransitionSet setStartDelay(long startDelay) {
    return (TransitionSet) super.setStartDelay(startDelay);
  }
  @Override public TransitionSet setInterpolator(TimeInterpolator interpolator) {
    changeFlags |= FLAG_CHANGE_INTERPOLATOR;
    for (Transition transition : transitions) transition.setInterpolator(interpolator);
    return (TransitionSet) super.setInterpolator(interpolator);
  }
  @Override public void setPropagation(TransitionPropagation propagation) {
    super.setPropagation(propagation);
    changeFlags |= FLAG_CHANGE_PROPAGATION;
    for (Transition transition : transitions) transition.setPropagation(propagation);
  }
  @Override public void setPathMotion(PathMotion pathMotion) {
    super.setPathMotion(pathMotion);
    changeFlags |= FLAG_CHANGE_PATH_MOTION;
    for (Transition transition : transitions) transition.setPathMotion(pathMotion);
  }
  @Override public void setEpicenterCallback(EpicenterCallback callback) {
    super.setEpicenterCallback(callback);
    changeFlags |= FLAG_CHANGE_EPICENTER;
    for (Transition transition : transitions) transition.setEpicenterCallback(callback);
  }

  @Override public TransitionSet addTarget(View target) {
    for (Transition transition : transitions) transition.addTarget(target);
    return (TransitionSet) super.addTarget(target);
  }
  @Override public TransitionSet addTarget(int targetId) {
    for (Transition transition : transitions) transition.addTarget(targetId);
    return (TransitionSet) super.addTarget(targetId);
  }
  @Override public TransitionSet addTarget(String targetName) {
    for (Transition transition : transitions) transition.addTarget(targetName);
    return (TransitionSet) super.addTarget(targetName);
  }
  @Override public TransitionSet addTarget(Class targetType) {
    for (Transition transition : transitions) transition.addTarget(targetType);
    return (TransitionSet) super.addTarget(targetType);
  }
  @Override public TransitionSet removeTarget(int targetId) {
    for (Transition transition : transitions) transition.removeTarget(targetId);
    return (TransitionSet) super.removeTarget(targetId);
  }
  @Override public TransitionSet removeTarget(View target) {
    for (Transition transition : transitions) transition.removeTarget(target);
    return (TransitionSet) super.removeTarget(target);
  }
  @Override public TransitionSet removeTarget(Class target) {
    for (Transition transition : transitions) transition.removeTarget(target);
    return (TransitionSet) super.removeTarget(target);
  }
  @Override public TransitionSet removeTarget(String target) {
    for (Transition transition : transitions) transition.removeTarget(target);
    return (TransitionSet) super.removeTarget(target);
  }
  @Override public Transition excludeTarget(View target, boolean exclude) {
    for (Transition transition : transitions) transition.excludeTarget(target, exclude);
    return super.excludeTarget(target, exclude);
  }
  @Override public Transition excludeTarget(String targetName, boolean exclude) {
    for (Transition transition : transitions) transition.excludeTarget(targetName, exclude);
    return super.excludeTarget(targetName, exclude);
  }
  @Override public Transition excludeTarget(int targetId, boolean exclude) {
    for (Transition transition : transitions) transition.excludeTarget(targetId, exclude);
    return super.excludeTarget(targetId, exclude);
  }
  @Override public Transition excludeTarget(Class type, boolean exclude) {
    for (Transition transition : transitions) transition.excludeTarget(type, exclude);
    return super.excludeTarget(type, exclude);
  }
  @Override public TransitionSet addListener(TransitionListener listener) {
    return (TransitionSet) super.addListener(listener);
  }
  @Override public TransitionSet removeListener(TransitionListener listener) {
    return (TransitionSet) super.removeListener(listener);
  }

  @Override public void captureStartValues(TransitionValues transitionValues) {
    if (isValidTarget(transitionValues.view))
      for (Transition child : transitions)
        if (child.isValidTarget(transitionValues.view)) child.captureStartValues(transitionValues);
  }
  @Override public void captureEndValues(TransitionValues transitionValues) {
    if (isValidTarget(transitionValues.view))
      for (Transition child : transitions)
        if (child.isValidTarget(transitionValues.view)) child.captureEndValues(transitionValues);
  }
  @Override void setCanRemoveViews(boolean canRemoveViews) {
    super.setCanRemoveViews(canRemoveViews);
    for (Transition transition : transitions) transition.setCanRemoveViews(canRemoveViews);
  }
  @Override TransitionSet setSceneRoot(ViewGroup sceneRoot) {
    super.setSceneRoot(sceneRoot);
    for (Transition transition : transitions) transition.setSceneRoot(sceneRoot);
    return this;
  }

  // AOSP TransitionSet.runAnimators with its TransitionSetListener: the set
  // starts with its first child and ends with its last; sequential children
  // run one after the other.
  @Override void runAnimators() {
    if (transitions.isEmpty()) {
      start();
      end();
      return;
    }
    var setListener = new TransitionListenerAdapter() {
      @Override public void onTransitionStart(Transition transition) {
        if (!started) {
          start();
          started = true;
        }
      }
      @Override public void onTransitionEnd(Transition transition) {
        if (--currentListeners == 0) {
          started = false;
          end();
        }
        transition.removeListener(this);
      }
    };
    for (Transition child : transitions) child.addListener(setListener);
    currentListeners = transitions.size();
    if (!playTogether) {
      for (int i = 1; i < transitions.size(); i++) {
        Transition next = transitions.get(i);
        transitions.get(i - 1).addListener(new TransitionListenerAdapter() {
          @Override public void onTransitionEnd(Transition transition) {
            next.runAnimators();
            transition.removeListener(this);
          }
        });
      }
      transitions.get(0).runAnimators();
    } else {
      for (Transition child : transitions) child.runAnimators();
    }
  }
  @Override public TransitionSet clone() {
    TransitionSet clone = (TransitionSet) super.clone();
    clone.transitions = new ArrayList<>();
    clone.currentListeners = 0;
    clone.started = false;
    for (Transition transition : transitions) clone.transitions.add(transition.clone());
    return clone;
  }
  @Override String toString(String indent) {
    var result = new StringBuilder(super.toString(indent));
    for (Transition transition : transitions) result.append("\n").append(transition.toString(indent + "  "));
    return result.toString();
  }
}
