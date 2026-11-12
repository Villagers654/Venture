package android.app;

/**
 * AOSP android.app.GameManager (API 31), Context.GAME_SERVICE. GameManagerService
 * answers GAME_MODE_UNSUPPORTED for a package that has no game mode configuration
 * on the device, which is every app on a headset without a game dashboard: the
 * game runs with no mode applied. setGameState (API 33) is a hint the service
 * may use for performance tuning; nothing here consumes it.
 */
public final class GameManager {
  public static final int GAME_MODE_UNSUPPORTED = 0;
  public static final int GAME_MODE_STANDARD = 1;
  public static final int GAME_MODE_PERFORMANCE = 2;
  public static final int GAME_MODE_BATTERY = 3;
  public GameManager(android.content.Context context) {}
  public int getGameMode() { return GAME_MODE_UNSUPPORTED; }
  public void setGameState(GameState gameState) { java.util.Objects.requireNonNull(gameState); }
}
