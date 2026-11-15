package android.transition;

import android.content.Context;
import android.util.SparseArray;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import java.util.Map;
import java.util.WeakHashMap;

/**
 * AOSP core/java/android/transition/Scene.java: the state of a scene root's view hierarchy. AOSP
 * keeps the current scene and the layout scene cache in view tags; Venture keeps them per root
 * in weak maps.
 */
public final class Scene {
  private static final Map<View, Scene> CURRENT = new WeakHashMap<>();
  private static final Map<ViewGroup, SparseArray<Scene>> LAYOUT_SCENES = new WeakHashMap<>();
  private final int layoutId;
  private final Context context;
  private final ViewGroup sceneRoot;
  private final View layout;
  private Runnable enterAction, exitAction;

  public static Scene getSceneForLayout(ViewGroup sceneRoot, int layoutId, Context context) {
    synchronized (LAYOUT_SCENES) {
      SparseArray<Scene> scenes = LAYOUT_SCENES.computeIfAbsent(sceneRoot, root -> new SparseArray<>());
      Scene scene = scenes.get(layoutId);
      if (scene == null) {
        scene = new Scene(sceneRoot, layoutId, context);
        scenes.put(layoutId, scene);
      }
      return scene;
    }
  }
  public Scene(ViewGroup sceneRoot) { this(sceneRoot, -1, null, null); }
  public Scene(ViewGroup sceneRoot, View layout) { this(sceneRoot, -1, null, layout); }
  @Deprecated public Scene(ViewGroup sceneRoot, ViewGroup layout) { this(sceneRoot, -1, null, layout); }
  private Scene(ViewGroup sceneRoot, int layoutId, Context context) { this(sceneRoot, layoutId, context, null); }
  private Scene(ViewGroup sceneRoot, int layoutId, Context context, View layout) {
    this.sceneRoot = sceneRoot;
    this.layoutId = layoutId;
    this.context = context;
    this.layout = layout;
  }

  public ViewGroup getSceneRoot() { return sceneRoot; }
  public void exit() {
    if (getCurrentScene(sceneRoot) == this && exitAction != null) exitAction.run();
  }
  public void enter() {
    if (layoutId > 0 || layout != null) {
      getSceneRoot().removeAllViews();
      if (layoutId > 0) LayoutInflater.from(context).inflate(layoutId, sceneRoot);
      else sceneRoot.addView(layout);
    }
    if (enterAction != null) enterAction.run();
    setCurrentScene(sceneRoot, this);
  }
  public void setEnterAction(Runnable action) { enterAction = action; }
  public void setExitAction(Runnable action) { exitAction = action; }
  boolean isCreatedFromLayoutResource() { return layoutId > 0; }

  static void setCurrentScene(View view, Scene scene) {
    synchronized (CURRENT) {
      if (scene == null) CURRENT.remove(view);
      else CURRENT.put(view, scene);
    }
  }
  static Scene getCurrentScene(View view) {
    synchronized (CURRENT) {
      return CURRENT.get(view);
    }
  }
}
