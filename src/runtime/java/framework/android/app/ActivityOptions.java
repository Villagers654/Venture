package android.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Rect;
import android.os.Bundle;
import android.util.Pair;
import android.view.View;
import android.view.Window;
import java.util.ArrayList;

/**
 * AOSP core/java/android/app/ActivityOptions.java: launch animation and placement options and
 * their Bundle form. Venture hosts one task on one display and runs no window animations, so a
 * launch accepts these options and they change nothing.
 */
public class ActivityOptions {
  static final String KEY_PACKAGE_NAME = "android:activity.packageName";
  static final String KEY_LAUNCH_BOUNDS = "android:activity.launchBounds";
  static final String KEY_ANIM_TYPE = "android:activity.animType";
  static final String KEY_ANIM_ENTER_RES_ID = "android:activity.animEnterRes";
  static final String KEY_ANIM_EXIT_RES_ID = "android:activity.animExitRes";
  static final String KEY_ANIM_START_X = "android:activity.animStartX";
  static final String KEY_ANIM_START_Y = "android:activity.animStartY";
  static final String KEY_ANIM_WIDTH = "android:activity.animWidth";
  static final String KEY_ANIM_HEIGHT = "android:activity.animHeight";
  static final String KEY_USAGE_TIME_REPORT = "android:activity.usageTimeReport";
  static final String KEY_TRANSITION_IS_RETURNING = "android:activity.transitionIsReturning";
  static final String KEY_TRANSITION_SHARED_ELEMENTS = "android:activity.sharedElementNames";
  static final String KEY_EXIT_COORDINATOR_INDEX = "android:activity.exitCoordinatorIndex";
  static final String KEY_LAUNCH_DISPLAY_ID = "android.activity.launchDisplayId";
  static final String KEY_LOCK_TASK_MODE = "android:activity.lockTaskMode";
  static final int ANIM_UNDEFINED = -1, ANIM_CUSTOM = 1, ANIM_SCALE_UP = 2,
      ANIM_THUMBNAIL_SCALE_UP = 3, ANIM_THUMBNAIL_SCALE_DOWN = 4, ANIM_SCENE_TRANSITION = 5, ANIM_DEFAULT = 6,
      ANIM_LAUNCH_TASK_BEHIND = 7, ANIM_CLIP_REVEAL = 11;
  private static final int INVALID_DISPLAY = -1;

  private String packageName;
  private Rect launchBounds;
  private int animationType = ANIM_UNDEFINED;
  private int customEnterResId, customExitResId;
  private int startX, startY, width, height;
  private Bitmap thumbnail;
  private PendingIntent usageTimeReport;
  private ArrayList<String> sharedElementNames;
  private boolean isReturning;
  private int exitCoordinatorIndex;
  private int launchDisplayId = INVALID_DISPLAY;
  private boolean lockTaskMode;

  ActivityOptions() {}

  public static ActivityOptions makeBasic() { return new ActivityOptions(); }
  public static ActivityOptions makeCustomAnimation(Context context, int enterResId, int exitResId) {
    ActivityOptions opts = new ActivityOptions();
    opts.packageName = context.getPackageName();
    opts.animationType = ANIM_CUSTOM;
    opts.customEnterResId = enterResId;
    opts.customExitResId = exitResId;
    return opts;
  }
  public static ActivityOptions makeScaleUpAnimation(View source, int startX, int startY, int width, int height) {
    return makeRevealAnimation(source, startX, startY, width, height, ANIM_SCALE_UP);
  }
  public static ActivityOptions makeClipRevealAnimation(View source, int startX, int startY, int width, int height) {
    return makeRevealAnimation(source, startX, startY, width, height, ANIM_CLIP_REVEAL);
  }
  private static ActivityOptions makeRevealAnimation(View source, int startX, int startY, int width, int height, int type) {
    ActivityOptions opts = new ActivityOptions();
    opts.packageName = source.getContext().getPackageName();
    opts.animationType = type;
    int[] pts = new int[2];
    source.getLocationOnScreen(pts);
    opts.startX = pts[0] + startX;
    opts.startY = pts[1] + startY;
    opts.width = width;
    opts.height = height;
    return opts;
  }
  public static ActivityOptions makeThumbnailScaleUpAnimation(View source, Bitmap thumbnail, int startX, int startY) {
    ActivityOptions opts = new ActivityOptions();
    opts.packageName = source.getContext().getPackageName();
    opts.animationType = ANIM_THUMBNAIL_SCALE_UP;
    opts.thumbnail = thumbnail;
    int[] pts = new int[2];
    source.getLocationOnScreen(pts);
    opts.startX = pts[0] + startX;
    opts.startY = pts[1] + startY;
    return opts;
  }
  public static ActivityOptions makeTaskLaunchBehind() {
    ActivityOptions opts = new ActivityOptions();
    opts.animationType = ANIM_LAUNCH_TASK_BEHIND;
    return opts;
  }
  public static ActivityOptions makeSceneTransitionAnimation(Activity activity, View sharedElement, String sharedElementName) {
    return makeSceneTransitionAnimation(activity, Pair.create(sharedElement, sharedElementName));
  }
  // AOSP: a window without FEATURE_ACTIVITY_TRANSITIONS gets the default
  // animation; otherwise the options carry the shared element names.
  @SafeVarargs
  public static ActivityOptions makeSceneTransitionAnimation(Activity activity, Pair<View, String>... sharedElements) {
    ActivityOptions opts = new ActivityOptions();
    Window window = activity.getWindow();
    if (!window.hasFeature(Window.FEATURE_ACTIVITY_TRANSITIONS)) {
      opts.animationType = ANIM_DEFAULT;
      return opts;
    }
    opts.animationType = ANIM_SCENE_TRANSITION;
    ArrayList<String> names = new ArrayList<>();
    if (sharedElements != null) {
      for (Pair<View, String> sharedElement : sharedElements) {
        if (sharedElement.second == null) throw new IllegalArgumentException("Shared element name must not be null");
        names.add(sharedElement.second);
        if (sharedElement.first == null) throw new IllegalArgumentException("Shared element must not be null");
      }
    }
    opts.sharedElementNames = names;
    opts.isReturning = false;
    return opts;
  }

  public ActivityOptions setLaunchBounds(Rect screenSpacePixelRect) {
    launchBounds = screenSpacePixelRect != null ? new Rect(screenSpacePixelRect) : null;
    return this;
  }
  public Rect getLaunchBounds() { return launchBounds; }
  public ActivityOptions setLaunchDisplayId(int launchDisplayId) {
    this.launchDisplayId = launchDisplayId;
    return this;
  }
  public int getLaunchDisplayId() { return launchDisplayId; }
  public ActivityOptions setLockTaskEnabled(boolean lockTaskMode) {
    this.lockTaskMode = lockTaskMode;
    return this;
  }
  public boolean getLockTaskMode() { return lockTaskMode; }
  public void requestUsageTimeReport(PendingIntent receiver) {
    if (receiver == null) throw new IllegalArgumentException("receiver must be not null");
    usageTimeReport = receiver;
  }

  // AOSP update: the other options' package, usage report and animation replace these.
  public void update(ActivityOptions otherOptions) {
    if (otherOptions.packageName != null) packageName = otherOptions.packageName;
    usageTimeReport = otherOptions.usageTimeReport;
    sharedElementNames = null;
    isReturning = false;
    exitCoordinatorIndex = 0;
    animationType = otherOptions.animationType;
    switch (otherOptions.animationType) {
      case ANIM_CUSTOM -> {
        customEnterResId = otherOptions.customEnterResId;
        customExitResId = otherOptions.customExitResId;
        thumbnail = null;
      }
      case ANIM_SCALE_UP, ANIM_CLIP_REVEAL -> {
        startX = otherOptions.startX;
        startY = otherOptions.startY;
        width = otherOptions.width;
        height = otherOptions.height;
        thumbnail = null;
      }
      case ANIM_THUMBNAIL_SCALE_UP, ANIM_THUMBNAIL_SCALE_DOWN -> {
        thumbnail = otherOptions.thumbnail;
        startX = otherOptions.startX;
        startY = otherOptions.startY;
      }
      case ANIM_SCENE_TRANSITION -> {
        sharedElementNames = otherOptions.sharedElementNames;
        isReturning = otherOptions.isReturning;
        thumbnail = null;
        exitCoordinatorIndex = otherOptions.exitCoordinatorIndex;
      }
      default -> {}
    }
    lockTaskMode = otherOptions.lockTaskMode;
  }

  public Bundle toBundle() {
    Bundle b = new Bundle();
    if (packageName != null) b.putString(KEY_PACKAGE_NAME, packageName);
    if (launchBounds != null) b.putParcelable(KEY_LAUNCH_BOUNDS, launchBounds);
    if (animationType != ANIM_UNDEFINED) b.putInt(KEY_ANIM_TYPE, animationType);
    if (usageTimeReport != null) b.putParcelable(KEY_USAGE_TIME_REPORT, usageTimeReport);
    switch (animationType) {
      case ANIM_CUSTOM -> {
        b.putInt(KEY_ANIM_ENTER_RES_ID, customEnterResId);
        b.putInt(KEY_ANIM_EXIT_RES_ID, customExitResId);
      }
      case ANIM_SCALE_UP, ANIM_CLIP_REVEAL, ANIM_THUMBNAIL_SCALE_UP, ANIM_THUMBNAIL_SCALE_DOWN -> {
        b.putInt(KEY_ANIM_START_X, startX);
        b.putInt(KEY_ANIM_START_Y, startY);
        b.putInt(KEY_ANIM_WIDTH, width);
        b.putInt(KEY_ANIM_HEIGHT, height);
      }
      case ANIM_SCENE_TRANSITION -> {
        b.putBoolean(KEY_TRANSITION_IS_RETURNING, isReturning);
        b.putStringArrayList(KEY_TRANSITION_SHARED_ELEMENTS, sharedElementNames);
        b.putInt(KEY_EXIT_COORDINATOR_INDEX, exitCoordinatorIndex);
      }
      default -> {}
    }
    if (lockTaskMode) b.putBoolean(KEY_LOCK_TASK_MODE, lockTaskMode);
    if (launchDisplayId != INVALID_DISPLAY) b.putInt(KEY_LAUNCH_DISPLAY_ID, launchDisplayId);
    return b;
  }

  @Override public String toString() {
    return "ActivityOptions(" + hashCode() + "), mPackageName=" + packageName + ", mAnimationType=" + animationType
        + ", mStartX=" + startX + ", mStartY=" + startY + ", mWidth=" + width + ", mHeight=" + height;
  }
}
