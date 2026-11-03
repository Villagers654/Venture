package android.content;
public class ContentResolver {
  public static final String SCHEME_CONTENT = "content";
  public static final String SCHEME_ANDROID_RESOURCE = "android.resource";
  public static final String SCHEME_FILE = "file";
  public static final String QUERY_ARG_SQL_SELECTION = "android:query-arg-sql-selection";
  public static final String QUERY_ARG_SQL_SELECTION_ARGS = "android:query-arg-sql-selection-args";
  public static final String QUERY_ARG_SQL_SORT_ORDER = "android:query-arg-sql-sort-order";
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
  /** Provider queries; null when no provider serves the authority, as on Android. */
  public android.database.Cursor query(android.net.Uri uri, String[] projection, String selection,
      String[] selectionArgs, String sortOrder) {
    java.util.Objects.requireNonNull(uri);
    if (!"content".equals(uri.getScheme()) || uri.getAuthority() == null) return null;
    var provider = com.venture.runtime.RuntimeEnvironment.get().contentProviders.get(uri.getAuthority());
    return provider == null ? null : provider.query(uri, projection, selection, selectionArgs, sortOrder);
  }
  private static ContentProvider provider(android.net.Uri uri) {
    java.util.Objects.requireNonNull(uri);
    if (!"content".equals(uri.getScheme()) || uri.getAuthority() == null) return null;
    return com.venture.runtime.RuntimeEnvironment.get().contentProviders.get(uri.getAuthority());
  }
  /** Writes to an in-process provider; Android fails an unknown authority with IllegalArgumentException. */
  private static ContentProvider writable(android.net.Uri uri) {
    var provider = provider(uri);
    if (provider == null) throw new IllegalArgumentException("Unknown URL " + uri);
    return provider;
  }
  public final android.net.Uri insert(android.net.Uri uri, ContentValues values) {
    return writable(uri).insert(uri, values);
  }
  public final int bulkInsert(android.net.Uri uri, ContentValues[] values) {
    return writable(uri).bulkInsert(uri, values);
  }
  public final int update(android.net.Uri uri, ContentValues values, String selection, String[] selectionArgs) {
    return writable(uri).update(uri, values, selection, selectionArgs);
  }
  public final int delete(android.net.Uri uri, String selection, String[] selectionArgs) {
    return writable(uri).delete(uri, selection, selectionArgs);
  }
  public final android.os.Bundle call(android.net.Uri uri, String method, String arg, android.os.Bundle extras) {
    return writable(uri).call(uri.getAuthority(), method, arg, extras);
  }
}
