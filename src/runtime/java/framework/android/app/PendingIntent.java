package android.app;

import android.content.Context;
import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.os.Parcel;
import android.os.Parcelable;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;

/** Process-local activity tokens. Cross-process services and broadcasts are unavailable. */
public final class PendingIntent implements Parcelable {
  public static final int FLAG_ONE_SHOT = 0x40000000, FLAG_NO_CREATE = 0x20000000,
      FLAG_CANCEL_CURRENT = 0x10000000, FLAG_UPDATE_CURRENT = 0x08000000,
      FLAG_IMMUTABLE = 0x04000000, FLAG_MUTABLE = 0x02000000;
  // Android Intent.filterEquals excludes extras and launch flags from token identity.
  private record Key(String creator, int request, String action, String data, String type,
      String pkg, android.content.ComponentName component, Set<String> categories, int identityFlags) {}
  private static final Map<Key, PendingIntent> tokens = new HashMap<>();
  private final Key key;
  private final Context context;
  private Intent intent;
  private boolean cancelled;
  private PendingIntent(Key k, Context c, Intent i) { key = k; context = c; intent = new Intent(i); }
  public static PendingIntent getActivity(Context c, int request, Intent i, int flags) {
    return getActivity(c, request, i, flags, null);
  }
  public static PendingIntent getActivity(Context c, int request, Intent i, int flags, Bundle options) {
    if (options != null && !options.isEmpty()) throw new UnsupportedOperationException("Activity options");
    if ((flags & FLAG_IMMUTABLE) != 0 && (flags & FLAG_MUTABLE) != 0)
      throw new IllegalArgumentException("Conflicting mutability flags");
    int supported = FLAG_ONE_SHOT | FLAG_NO_CREATE | FLAG_CANCEL_CURRENT | FLAG_UPDATE_CURRENT
        | FLAG_IMMUTABLE | FLAG_MUTABLE;
    if ((flags & ~supported) != 0) throw new UnsupportedOperationException("Intent fill-in flags");
    Key key = new Key(c.getPackageName(), request, i.getAction(), i.getDataString(), i.getType(),
        i.getPackage(), i.getComponent(), i.getCategories(), flags & (FLAG_ONE_SHOT | FLAG_IMMUTABLE | FLAG_MUTABLE));
    synchronized (tokens) {
      PendingIntent value = tokens.get(key);
      if (value != null && (flags & FLAG_CANCEL_CURRENT) != 0) { value.cancel(); value = null; }
      if (value != null) {
        if ((flags & FLAG_UPDATE_CURRENT) != 0) value.intent = new Intent(i);
        return value;
      }
      if ((flags & FLAG_NO_CREATE) != 0) return null;
      value = new PendingIntent(key, c, i);
      tokens.put(key, value);
      return value;
    }
  }
  public static PendingIntent getBroadcast(Context c, int request, Intent i, int flags) {
    throw new UnsupportedOperationException("Broadcast PendingIntent delivery unavailable");
  }
  public static PendingIntent getService(Context c, int request, Intent i, int flags) {
    throw new UnsupportedOperationException("Service PendingIntent delivery unavailable");
  }
  public void cancel() {
    synchronized (tokens) { cancelled = true; tokens.remove(key, this); }
  }
  public String getCreatorPackage() { return key.creator(); }
  public android.content.IntentSender getIntentSender() { return new android.content.IntentSender(this); }
  public String getTargetPackage() { return getCreatorPackage(); }
  public int getCreatorUid() { return android.os.Process.myUid(); }
  public boolean isActivity() { return true; }
  public boolean isImmutable() { return (key.identityFlags() & FLAG_IMMUTABLE) != 0; }
  public void send() throws CanceledException { send(0); }
  public void send(int code) throws CanceledException { send(null, code, null); }
  public void send(Context c, int code, Intent fill) throws CanceledException { send(c, code, fill, null, null); }
  public void send(Context c, int code, Intent fill, OnFinished callback, Handler handler) throws CanceledException {
    Intent deliver;
    synchronized (tokens) {
      if (cancelled) throw new CanceledException();
      if (fill != null && !isImmutable())
        throw new UnsupportedOperationException("Mutable PendingIntent fill-in unavailable");
      deliver = new Intent(intent);
      if ((key.identityFlags() & FLAG_ONE_SHOT) != 0) cancel();
    }
    context.startActivity(deliver);
    if (callback != null) {
      Runnable complete = () -> callback.onSendFinished(this, deliver, code, null, null);
      if (handler == null) complete.run(); else handler.post(complete);
    }
  }
  public interface OnFinished {
    void onSendFinished(PendingIntent token, Intent intent, int resultCode, String resultData, Bundle resultExtras);
  }
  public static class CanceledException extends Exception {
    public CanceledException() {}
    public CanceledException(String message) { super(message); }
  }
  public int describeContents() { return 0; }
  public void writeToParcel(Parcel parcel, int flags) { parcel.writeParcelable(this, flags); }
  public static final Parcelable.Creator<PendingIntent> CREATOR = new Parcelable.Creator<>() {
    public PendingIntent createFromParcel(Parcel p) { return p.readParcelable(null); }
    public PendingIntent[] newArray(int size) { return new PendingIntent[size]; }
  };
}
