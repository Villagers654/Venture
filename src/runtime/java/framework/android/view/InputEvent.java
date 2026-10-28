package android.view;
public abstract class InputEvent {
  public abstract int getSource();
  public abstract int getDeviceId();
  public abstract long getEventTime();
}
