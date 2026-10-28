package android.widget;
public class MediaController extends FrameLayout {
  public MediaController(android.content.Context context) {
    super(context);
    throw new UnsupportedOperationException("Host media controls are unavailable");
  }
  public interface MediaPlayerControl {
    void start(); void pause(); int getDuration(); int getCurrentPosition();
    void seekTo(int position); boolean isPlaying(); int getBufferPercentage();
    boolean canPause(); boolean canSeekBackward(); boolean canSeekForward();
    int getAudioSessionId();
  }
}
