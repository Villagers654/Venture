package android.transition;

import android.animation.Animator;
import android.animation.TimeInterpolator;
import android.content.Context;
import android.graphics.Rect;
import android.util.AttributeSet;
import android.view.View;
import android.view.ViewGroup;
import java.util.ArrayList;
import java.util.List;

/**
 * AOSP core/java/android/transition/Transition.java: targets, exclusions, timing, propagation,
 * path motion, epicenter and listeners as on Android. Venture has no host renderer for Java
 * views, so a transition that runs creates no animators: it starts and ends on the frame it
 * would have played (see TransitionManager), the view changes already applied.
 */
public abstract class Transition implements Cloneable {
  public static final int MATCH_INSTANCE = 0x1, MATCH_NAME = 0x2, MATCH_ID = 0x3, MATCH_ITEM_ID = 0x4;
  private static final int MATCH_FIRST = MATCH_INSTANCE, MATCH_LAST = MATCH_ITEM_ID;
  private static final int[] DEFAULT_MATCH_ORDER = {MATCH_NAME, MATCH_INSTANCE, MATCH_ID, MATCH_ITEM_ID};
  private static final PathMotion STRAIGHT_PATH_MOTION = new PathMotion() {
    @Override public android.graphics.Path getPath(float startX, float startY, float endX, float endY) {
      var path = new android.graphics.Path();
      path.moveTo(startX, startY);
      path.lineTo(endX, endY);
      return path;
    }
  };

  private String name = getClass().getName();
  long startDelay = -1, duration = -1;
  TimeInterpolator interpolator;
  ArrayList<Integer> targetIds = new ArrayList<>();
  ArrayList<View> targets = new ArrayList<>();
  ArrayList<String> targetNames;
  ArrayList<Class<?>> targetTypes;
  private ArrayList<Integer> targetIdExcludes, targetIdChildExcludes;
  private ArrayList<View> targetExcludes, targetChildExcludes;
  private ArrayList<Class<?>> targetTypeExcludes, targetTypeChildExcludes;
  private ArrayList<String> targetNameExcludes;
  private int[] matchOrder = DEFAULT_MATCH_ORDER;
  ArrayList<TransitionListener> listeners;
  ViewGroup sceneRoot;
  boolean canRemoveViews;
  TransitionPropagation propagation;
  EpicenterCallback epicenterCallback;
  PathMotion pathMotion = STRAIGHT_PATH_MOTION;
  private int numInstances;

  public Transition() {}
  public Transition(Context context, AttributeSet attrs) {}

  public Transition setDuration(long duration) { this.duration = duration; return this; }
  public long getDuration() { return duration; }
  public Transition setStartDelay(long startDelay) { this.startDelay = startDelay; return this; }
  public long getStartDelay() { return startDelay; }
  public Transition setInterpolator(TimeInterpolator interpolator) { this.interpolator = interpolator; return this; }
  public TimeInterpolator getInterpolator() { return interpolator; }
  public String[] getTransitionProperties() { return null; }
  public String getName() { return name; }

  public Animator createAnimator(ViewGroup sceneRoot, TransitionValues startValues, TransitionValues endValues) {
    return null;
  }
  public abstract void captureStartValues(TransitionValues transitionValues);
  public abstract void captureEndValues(TransitionValues transitionValues);

  // AOSP: an empty order restores the default; repeats and unknown values are rejected.
  public void setMatchOrder(int... matches) {
    if (matches == null || matches.length == 0) {
      matchOrder = DEFAULT_MATCH_ORDER;
      return;
    }
    for (int i = 0; i < matches.length; i++) {
      if (matches[i] < MATCH_FIRST || matches[i] > MATCH_LAST)
        throw new IllegalArgumentException("matches contains invalid value");
      for (int j = 0; j < i; j++)
        if (matches[j] == matches[i]) throw new IllegalArgumentException("matches contains a duplicate value");
    }
    matchOrder = matches.clone();
  }

  public boolean isTransitionRequired(TransitionValues startValues, TransitionValues endValues) {
    if (startValues == null || endValues == null) return false;
    String[] properties = getTransitionProperties();
    if (properties != null) {
      for (String property : properties)
        if (isValueChanged(startValues, endValues, property)) return true;
      return false;
    }
    for (String key : startValues.values.keySet())
      if (isValueChanged(startValues, endValues, key)) return true;
    return false;
  }
  private static boolean isValueChanged(TransitionValues oldValues, TransitionValues newValues, String key) {
    if (oldValues.values.containsKey(key) != newValues.values.containsKey(key)) return false;
    Object oldValue = oldValues.values.get(key), newValue = newValues.values.get(key);
    if (oldValue == null && newValue == null) return false;
    if (oldValue == null || newValue == null) return true;
    return !oldValue.equals(newValue);
  }

  // AOSP isValidTarget: exclusions first, then the explicit targets, if any.
  boolean isValidTarget(View target) {
    if (target == null) return false;
    int targetId = target.getId();
    if (targetIdExcludes != null && targetIdExcludes.contains(targetId)) return false;
    if (targetExcludes != null && targetExcludes.contains(target)) return false;
    if (targetTypeExcludes != null)
      for (Class<?> type : targetTypeExcludes) if (type.isInstance(target)) return false;
    if (targetNameExcludes != null && target.getTransitionName() != null
        && targetNameExcludes.contains(target.getTransitionName()))
      return false;
    if (targetIds.isEmpty() && targets.isEmpty() && (targetTypes == null || targetTypes.isEmpty())
        && (targetNames == null || targetNames.isEmpty()))
      return true;
    if (targetIds.contains(targetId) || targets.contains(target)) return true;
    if (targetNames != null && targetNames.contains(target.getTransitionName())) return true;
    if (targetTypes != null)
      for (Class<?> type : targetTypes) if (type.isInstance(target)) return true;
    return false;
  }

  public Transition addTarget(int targetId) {
    if (targetId != 0) targetIds.add(targetId);
    return this;
  }
  public Transition addTarget(String targetName) {
    if (targetName != null) {
      if (targetNames == null) targetNames = new ArrayList<>();
      targetNames.add(targetName);
    }
    return this;
  }
  public Transition addTarget(Class targetType) {
    if (targetType != null) {
      if (targetTypes == null) targetTypes = new ArrayList<>();
      targetTypes.add(targetType);
    }
    return this;
  }
  public Transition addTarget(View target) {
    targets.add(target);
    return this;
  }
  public Transition removeTarget(int targetId) {
    if (targetId != 0) targetIds.remove((Integer) targetId);
    return this;
  }
  public Transition removeTarget(String targetName) {
    if (targetName != null && targetNames != null) targetNames.remove(targetName);
    return this;
  }
  public Transition removeTarget(View target) {
    if (target != null) targets.remove(target);
    return this;
  }
  public Transition removeTarget(Class target) {
    if (target != null && targetTypes != null) targetTypes.remove(target);
    return this;
  }
  public List<Integer> getTargetIds() { return targetIds; }
  public List<View> getTargets() { return targets; }
  public List<String> getTargetNames() { return targetNames; }
  public List<Class> getTargetTypes() { return (List) targetTypes; }

  private static <T> ArrayList<T> exclude(ArrayList<T> list, T target, boolean exclude) {
    if (target != null) {
      if (exclude) {
        if (list == null) list = new ArrayList<>();
        if (!list.contains(target)) list.add(target);
      } else if (list != null) {
        list.remove(target);
        if (list.isEmpty()) list = null;
      }
    }
    return list;
  }
  public Transition excludeTarget(int targetId, boolean exclude) {
    if (targetId >= 0) targetIdExcludes = exclude(targetIdExcludes, targetId, exclude);
    return this;
  }
  public Transition excludeTarget(String targetName, boolean exclude) {
    targetNameExcludes = exclude(targetNameExcludes, targetName, exclude);
    return this;
  }
  public Transition excludeTarget(View target, boolean exclude) {
    targetExcludes = exclude(targetExcludes, target, exclude);
    return this;
  }
  public Transition excludeTarget(Class type, boolean exclude) {
    targetTypeExcludes = exclude(targetTypeExcludes, (Class<?>) type, exclude);
    return this;
  }
  public Transition excludeChildren(int targetId, boolean exclude) {
    if (targetId >= 0) targetIdChildExcludes = exclude(targetIdChildExcludes, targetId, exclude);
    return this;
  }
  public Transition excludeChildren(View target, boolean exclude) {
    targetChildExcludes = exclude(targetChildExcludes, target, exclude);
    return this;
  }
  public Transition excludeChildren(Class type, boolean exclude) {
    targetTypeChildExcludes = exclude(targetTypeChildExcludes, (Class<?>) type, exclude);
    return this;
  }

  public Transition addListener(TransitionListener listener) {
    if (listeners == null) listeners = new ArrayList<>();
    listeners.add(listener);
    return this;
  }
  public Transition removeListener(TransitionListener listener) {
    if (listeners == null) return this;
    listeners.remove(listener);
    if (listeners.isEmpty()) listeners = null;
    return this;
  }
  public void setEpicenterCallback(EpicenterCallback epicenterCallback) { this.epicenterCallback = epicenterCallback; }
  public EpicenterCallback getEpicenterCallback() { return epicenterCallback; }
  public Rect getEpicenter() { return epicenterCallback == null ? null : epicenterCallback.onGetEpicenter(this); }
  public void setPathMotion(PathMotion pathMotion) {
    this.pathMotion = pathMotion == null ? STRAIGHT_PATH_MOTION : pathMotion;
  }
  public PathMotion getPathMotion() { return pathMotion; }
  public void setPropagation(TransitionPropagation transitionPropagation) { propagation = transitionPropagation; }
  public TransitionPropagation getPropagation() { return propagation; }
  public boolean canRemoveViews() { return canRemoveViews; }
  void setCanRemoveViews(boolean canRemoveViews) { this.canRemoveViews = canRemoveViews; }
  Transition setSceneRoot(ViewGroup sceneRoot) { this.sceneRoot = sceneRoot; return this; }

  // AOSP Transition.runAnimators: start(), run each animator, end(). There are no
  // animators, so a run starts and ends at once.
  void runAnimators() {
    start();
    end();
  }
  // AOSP Transition.start/end: listeners see the first start and the last end.
  void start() {
    if (numInstances == 0) {
      for (TransitionListener listener : snapshot(listeners)) listener.onTransitionStart(this);
    }
    numInstances++;
  }
  void end() {
    if (--numInstances == 0) {
      for (TransitionListener listener : snapshot(listeners)) listener.onTransitionEnd(this);
    }
  }
  static ArrayList<TransitionListener> snapshot(ArrayList<TransitionListener> listeners) {
    return listeners == null ? new ArrayList<>() : new ArrayList<>(listeners);
  }

  @Override public Transition clone() {
    try {
      Transition clone = (Transition) super.clone();
      clone.targetIds = new ArrayList<>(targetIds);
      clone.targets = new ArrayList<>(targets);
      if (targetNames != null) clone.targetNames = new ArrayList<>(targetNames);
      if (targetTypes != null) clone.targetTypes = new ArrayList<>(targetTypes);
      if (listeners != null) clone.listeners = new ArrayList<>(listeners);
      clone.numInstances = 0;
      return clone;
    } catch (CloneNotSupportedException e) {
      throw new AssertionError(e);
    }
  }

  @Override public String toString() { return toString(""); }
  String toString(String indent) {
    var result = new StringBuilder(indent).append(getClass().getSimpleName()).append("@")
        .append(Integer.toHexString(hashCode())).append(": ");
    if (duration != -1) result.append("dur(").append(duration).append(") ");
    if (startDelay != -1) result.append("dly(").append(startDelay).append(") ");
    if (interpolator != null) result.append("interp(").append(interpolator).append(") ");
    if (!targetIds.isEmpty() || !targets.isEmpty()) {
      result.append("tgts(");
      String separator = "";
      for (Integer id : targetIds) { result.append(separator).append(id); separator = ", "; }
      for (View target : targets) { result.append(separator).append(target); separator = ", "; }
      result.append(")");
    }
    return result.toString();
  }

  public interface TransitionListener {
    void onTransitionStart(Transition transition);
    void onTransitionEnd(Transition transition);
    void onTransitionCancel(Transition transition);
    void onTransitionPause(Transition transition);
    void onTransitionResume(Transition transition);
  }

  public static abstract class EpicenterCallback {
    public abstract Rect onGetEpicenter(Transition transition);
  }
}
