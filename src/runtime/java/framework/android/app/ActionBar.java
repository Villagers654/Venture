package android.app;
public abstract class ActionBar {
  public abstract void setTitle(CharSequence title);
  public abstract void show();
  public abstract void hide();
  public abstract boolean isShowing();
}
