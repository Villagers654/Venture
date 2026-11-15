package android.transition;

import android.os.Handler;
import android.os.Looper;
import android.view.ViewGroup;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.Map;

/**
 * AOSP core/java/android/transition/TransitionManager.java. Scene changes apply at once, as on
 * Android. AOSP then plays the transition at the scene root's next pre-draw; Venture has no host
 * renderer for Java views, so it runs the transition from the calling thread's Looper instead,
 * and with no animators it starts and ends there (listeners see both). A root waiting for its
 * transition ignores further scene changes and delayed transitions until it runs, and
 * endTransitions cancels a waiting one, as on AOSP.
 */
public class TransitionManager {
  private static final Transition DEFAULT_TRANSITION = new AutoTransition();
  private static final ArrayList<ViewGroup> PENDING = new ArrayList<>();
  private final Map<Scene, Transition> sceneTransitions = new HashMap<>();
  private final Map<Scene, Map<Scene, Transition>> scenePairTransitions = new HashMap<>();

  public TransitionManager() {}

  public void setTransition(Scene scene, Transition transition) { sceneTransitions.put(scene, transition); }
  public void setTransition(Scene fromScene, Scene toScene, Transition transition) {
    scenePairTransitions.computeIfAbsent(toScene, scene -> new HashMap<>()).put(fromScene, transition);
  }
  Transition getTransition(Scene scene) {
    ViewGroup sceneRoot = scene.getSceneRoot();
    if (sceneRoot != null) {
      Scene currentScene = Scene.getCurrentScene(sceneRoot);
      if (currentScene != null) {
        Map<Scene, Transition> fromTransitions = scenePairTransitions.get(scene);
        if (fromTransitions != null) {
          Transition transition = fromTransitions.get(currentScene);
          if (transition != null) return transition;
        }
      }
    }
    Transition transition = sceneTransitions.get(scene);
    return transition != null ? transition : DEFAULT_TRANSITION;
  }
  public void transitionTo(Scene scene) { changeScene(scene, getTransition(scene)); }

  public static void go(Scene scene) { changeScene(scene, DEFAULT_TRANSITION); }
  public static void go(Scene scene, Transition transition) { changeScene(scene, transition); }

  private static void changeScene(Scene scene, Transition transition) {
    ViewGroup sceneRoot = scene.getSceneRoot();
    if (isPending(sceneRoot)) return;
    Scene oldScene = Scene.getCurrentScene(sceneRoot);
    if (transition == null) {
      if (oldScene != null) oldScene.exit();
      scene.enter();
      return;
    }
    addPending(sceneRoot);
    Transition transitionClone = transition.clone();
    transitionClone.setSceneRoot(sceneRoot);
    if (oldScene != null && oldScene.isCreatedFromLayoutResource()) transitionClone.setCanRemoveViews(true);
    sceneChangeSetup(sceneRoot);
    scene.enter();
    sceneChangeRunTransition(sceneRoot, transitionClone);
  }

  public static void beginDelayedTransition(ViewGroup sceneRoot) { beginDelayedTransition(sceneRoot, null); }
  public static void beginDelayedTransition(ViewGroup sceneRoot, Transition transition) {
    if (isPending(sceneRoot) || !sceneRoot.isLaidOut()) return;
    addPending(sceneRoot);
    if (transition == null) transition = DEFAULT_TRANSITION;
    Transition transitionClone = transition.clone();
    sceneChangeSetup(sceneRoot);
    Scene.setCurrentScene(sceneRoot, null);
    sceneChangeRunTransition(sceneRoot, transitionClone);
  }

  public static void endTransitions(ViewGroup sceneRoot) {
    synchronized (PENDING) {
      PENDING.remove(sceneRoot);
    }
  }

  private static void sceneChangeSetup(ViewGroup sceneRoot) {
    Scene previousScene = Scene.getCurrentScene(sceneRoot);
    if (previousScene != null) previousScene.exit();
  }
  private static void sceneChangeRunTransition(ViewGroup sceneRoot, Transition transition) {
    Looper looper = Looper.myLooper();
    new Handler(looper != null ? looper : Looper.getMainLooper()).post(() -> {
      synchronized (PENDING) {
        if (!PENDING.remove(sceneRoot)) return;
      }
      transition.setSceneRoot(sceneRoot);
      transition.runAnimators();
    });
  }
  private static boolean isPending(ViewGroup sceneRoot) {
    synchronized (PENDING) {
      return PENDING.contains(sceneRoot);
    }
  }
  private static void addPending(ViewGroup sceneRoot) {
    synchronized (PENDING) {
      PENDING.add(sceneRoot);
    }
  }
}
