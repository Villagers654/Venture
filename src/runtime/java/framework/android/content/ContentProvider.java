package android.content;
public abstract class ContentProvider {
  private Context context;
  public abstract boolean onCreate();
  public abstract String getType(android.net.Uri uri);
  public final Context getContext() {
    return context;
  }
  public void attachInfo(Context c, android.content.pm.ProviderInfo info) {
    context = c;
    onCreate();
  }
}
