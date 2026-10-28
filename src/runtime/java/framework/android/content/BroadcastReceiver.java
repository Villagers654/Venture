package android.content;
public abstract class BroadcastReceiver {
  public abstract void onReceive(Context context, Intent intent);
  public boolean isInitialStickyBroadcast() {
    return true;
  }
  public boolean isOrderedBroadcast() {
    return false;
  }
}
