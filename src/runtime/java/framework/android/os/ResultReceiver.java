package android.os;
// In-process only: results run on the given handler's thread, or the caller's.
public class ResultReceiver implements Parcelable {
  private final Handler handler;
  public ResultReceiver(Handler handler) { this.handler = handler; }
  public void send(int resultCode, Bundle resultData) {
    if (handler != null) handler.post(() -> onReceiveResult(resultCode, resultData));
    else onReceiveResult(resultCode, resultData);
  }
  protected void onReceiveResult(int resultCode, Bundle resultData) {}
  public int describeContents() { return 0; }
  public void writeToParcel(Parcel out, int flags) {}
}
