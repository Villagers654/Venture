package android.app;

/**
 * AOSP android.app.GameState (API 33): what a game reports to GameManager.setGameState.
 * Engines link it next to GameManager even when they only call it on API 33+.
 */
public final class GameState {
  public static final int MODE_UNKNOWN = 0;
  public static final int MODE_NONE = 1;
  public static final int MODE_GAMEPLAY_INTERRUPTIBLE = 2;
  public static final int MODE_GAMEPLAY_UNINTERRUPTIBLE = 3;
  public static final int MODE_CONTENT = 4;
  private final boolean loading;
  private final int mode, label, quality;
  public GameState(boolean isLoading, int mode) { this(isLoading, mode, -1, -1); }
  public GameState(boolean isLoading, int mode, int label, int quality) {
    this.loading = isLoading;
    this.mode = mode;
    this.label = label;
    this.quality = quality;
  }
  public boolean isLoading() { return loading; }
  public int getMode() { return mode; }
  public int getLabel() { return label; }
  public int getQuality() { return quality; }
}
