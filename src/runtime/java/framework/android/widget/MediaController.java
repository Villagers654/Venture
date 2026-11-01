package android.widget;
public class MediaController extends FrameLayout {
  public MediaController(android.content.Context context) {
    super(context);
  }
  // Playback controls are not drawn (no Android view compositor); state is kept for callers.
  private MediaPlayerControl player;
  private android.view.View anchor;
  private boolean showing;
  public void setMediaPlayer(MediaPlayerControl player) { this.player = player; }
  public void setAnchorView(android.view.View view) { anchor = view; }
  public void show() { showing = true; }
  public void show(int timeout) { show(); }
  public void hide() { showing = false; }
  public boolean isShowing() { return showing; }
  public interface MediaPlayerControl {
    void start(); void pause(); int getDuration(); int getCurrentPosition();
    void seekTo(int position); boolean isPlaying(); int getBufferPercentage();
    boolean canPause(); boolean canSeekBackward(); boolean canSeekForward();
    int getAudioSessionId();
  }
}
