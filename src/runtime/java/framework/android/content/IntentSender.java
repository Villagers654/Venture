package android.content;

import android.app.PendingIntent;
import android.os.*;

/** Sender for a process-local PendingIntent; shares its cancellation and one-shot state. */
public final class IntentSender implements Parcelable {
  private final PendingIntent token;
  public IntentSender(PendingIntent token) { this.token = java.util.Objects.requireNonNull(token); }
  public void sendIntent(Context context, int code, Intent intent, OnFinished finished, Handler handler)
      throws SendIntentException {
    try {
      token.send(context, code, intent, finished == null ? null :
          (t, i, c, data, extras) -> finished.onSendFinished(this, i, c, data, extras), handler);
    } catch (PendingIntent.CanceledException e) { throw new SendIntentException(e); }
  }
  public String getCreatorPackage() { return token.getCreatorPackage(); }
  public String getTargetPackage() { return getCreatorPackage(); }
  public int getCreatorUid() { return token.getCreatorUid(); }
  public interface OnFinished {
    void onSendFinished(IntentSender sender, Intent intent, int resultCode, String resultData, Bundle resultExtras);
  }
  public static class SendIntentException extends Exception {
    public SendIntentException() {}
    public SendIntentException(String message) { super(message); }
    public SendIntentException(Exception cause) { super(cause); }
  }
  public boolean equals(Object other) { return other instanceof IntentSender s && token.equals(s.token); }
  public int hashCode() { return token.hashCode(); }
  public int describeContents() { return 0; }
  public void writeToParcel(Parcel dest, int flags) { token.writeToParcel(dest, flags); }
  public static final Parcelable.Creator<IntentSender> CREATOR = new Parcelable.Creator<>() {
    public IntentSender createFromParcel(Parcel p) { return new IntentSender(PendingIntent.CREATOR.createFromParcel(p)); }
    public IntentSender[] newArray(int size) { return new IntentSender[size]; }
  };
}
