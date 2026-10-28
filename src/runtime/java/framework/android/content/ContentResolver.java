package android.content;
public class ContentResolver {
  public String getType(android.net.Uri uri) {
    java.util.Objects.requireNonNull(uri);
    // AOSP ContentResolver delegates content MIME queries to the provider and
    // returns null when no provider handles the authority (e.g. absent GMS).
    if (!"content".equals(uri.getScheme()) || uri.getAuthority() == null) return null;
    var provider = com.venture.runtime.RuntimeEnvironment.get().contentProviders.get(uri.getAuthority());
    if (provider == null) return null;
    try {
      return provider.getType(uri);
    } catch (Exception failure) {
      // AOSP 12L ContentResolver.getType catches provider Exceptions, not Errors.
      // URI and provider messages may contain credentials; log only the type.
      android.util.Log.w("ContentResolver", "Provider MIME query failed (" + failure.getClass().getSimpleName() + ")");
      return null;
    }
  }
  private record
      Watch(android.net.Uri uri, boolean descendants, android.database.ContentObserver observer) {}
  private final java.util.List<Watch> watches = new java.util.concurrent.CopyOnWriteArrayList<>();
  public void registerContentObserver(
      android.net.Uri uri, boolean descendants, android.database.ContentObserver observer) {
    watches.add(new Watch(uri, descendants, observer));
  }
  public void unregisterContentObserver(android.database.ContentObserver observer) {
    watches.removeIf(w -> w.observer == observer);
  }
  public void notifyChange(android.net.Uri uri, android.database.ContentObserver source) {
    for (var w : watches)
      if ((w.observer != source || w.observer.deliverSelfNotifications())
          && (w.uri.equals(uri) || w.descendants && uri.toString().startsWith(w.uri + "/")))
        w.observer.dispatchChange(w.observer == source, uri);
  }
}
