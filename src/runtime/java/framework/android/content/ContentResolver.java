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
  // The Khronos OpenXR loader asks the runtime broker with the CancellationSignal overload.
  public final android.database.Cursor query(android.net.Uri uri, String[] projection, String selection,
      String[] selectionArgs, String sortOrder, android.os.CancellationSignal cancellationSignal) {
    if (cancellationSignal != null) cancellationSignal.throwIfCanceled();
    var provider = provider(uri);
    return provider == null ? null
        : provider.query(uri, projection, selection, selectionArgs, sortOrder, cancellationSignal);
  }
  public final android.database.Cursor query(android.net.Uri uri, String[] projection,
      android.os.Bundle queryArgs, android.os.CancellationSignal cancellationSignal) {
    if (cancellationSignal != null) cancellationSignal.throwIfCanceled();
    var provider = provider(uri);
    return provider == null ? null : provider.query(uri, projection, queryArgs, cancellationSignal);
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
  public void registerContentObserver(android.net.Uri uri, boolean descendants,
                                      android.database.ContentObserver observer, int userHandle) {
    registerContentObserver(uri, descendants, observer);
  }
  /** A resource named by an android.resource URI (ImageView.setImageURI and similar). */
  public class OpenResourceIdResult {
    public android.content.res.Resources r;
    public int id;
  }
  public OpenResourceIdResult getResourceId(android.net.Uri uri) throws java.io.FileNotFoundException {
    String authority = uri.getAuthority();
    if (authority == null || authority.isEmpty()) throw new java.io.FileNotFoundException("No authority: " + uri);
    boolean system = authority.equals("android");
    if (!system && !authority.equals(com.venture.runtime.RuntimeEnvironment.get().manifest.info.packageName))
      throw new java.io.FileNotFoundException("No package found for authority: " + uri);
    android.content.res.Resources resources =
        system ? android.content.res.Resources.getSystem() : com.venture.runtime.RuntimeEnvironment.resources();
    java.util.List<String> path = uri.getPathSegments();
    if (path == null) throw new java.io.FileNotFoundException("No path: " + uri);
    int id = 0;
    if (path.size() == 1) {
      try {
        id = Integer.parseInt(path.get(0));
      } catch (NumberFormatException e) {
        throw new java.io.FileNotFoundException("Single path segment is not a resource ID: " + uri);
      }
    } else if (path.size() == 2) {
      id = resources.getIdentifier(path.get(1), path.get(0), authority);
    } else {
      throw new java.io.FileNotFoundException("More than two path segments: " + uri);
    }
    if (id == 0) throw new java.io.FileNotFoundException("No resource found for: " + uri);
    OpenResourceIdResult result = new OpenResourceIdResult();
    result.r = resources;
    result.id = id;
    return result;
  }
  /** Files, the app's resources, and files the app's own providers open (openFile). */
  public final java.io.InputStream openInputStream(android.net.Uri uri) throws java.io.FileNotFoundException {
    java.util.Objects.requireNonNull(uri, "uri");
    String scheme = uri.getScheme();
    if (SCHEME_ANDROID_RESOURCE.equals(scheme)) {
      OpenResourceIdResult resource = getResourceId(uri);
      try {
        return resource.r.openRawResource(resource.id);
      } catch (RuntimeException e) {
        throw new java.io.FileNotFoundException("Resource does not exist: " + uri);
      }
    }
    if (SCHEME_FILE.equals(scheme) || scheme == null) return new java.io.FileInputStream(uri.getPath());
    var provider = provider(uri);
    if (provider == null) throw new java.io.FileNotFoundException("No content provider: " + uri);
    android.os.ParcelFileDescriptor descriptor = provider.openFile(uri, "r");
    if (descriptor == null) throw new java.io.FileNotFoundException("No file for " + uri);
    return new android.os.ParcelFileDescriptor.AutoCloseInputStream(descriptor);
  }
  /** Files and documents providers open: file URIs, and content URIs through the provider's openFile. */
  public final android.os.ParcelFileDescriptor openFileDescriptor(android.net.Uri uri, String mode)
      throws java.io.FileNotFoundException {
    return openFileDescriptor(uri, mode, null);
  }
  public final android.os.ParcelFileDescriptor openFileDescriptor(android.net.Uri uri, String mode,
      android.os.CancellationSignal signal) throws java.io.FileNotFoundException {
    java.util.Objects.requireNonNull(uri, "uri");
    if (signal != null) signal.throwIfCanceled();
    String scheme = uri.getScheme();
    if (SCHEME_FILE.equals(scheme))
      return android.os.ParcelFileDescriptor.open(new java.io.File(uri.getPath()),
          android.os.ParcelFileDescriptor.parseMode(mode));
    var provider = provider(uri);
    if (provider == null) throw new java.io.FileNotFoundException("No content provider: " + uri);
    android.os.ParcelFileDescriptor descriptor = provider.openFile(uri, mode);
    if (descriptor == null) throw new java.io.FileNotFoundException("No file for " + uri);
    return descriptor;
  }
  public final java.io.OutputStream openOutputStream(android.net.Uri uri) throws java.io.FileNotFoundException {
    return openOutputStream(uri, "w");
  }
  public final java.io.OutputStream openOutputStream(android.net.Uri uri, String mode)
      throws java.io.FileNotFoundException {
    return new android.os.ParcelFileDescriptor.AutoCloseOutputStream(openFileDescriptor(uri, mode));
  }
  /** A provider call by authority (DocumentsContract's document operations). */
  public final android.os.Bundle call(String authority, String method, String arg, android.os.Bundle extras) {
    java.util.Objects.requireNonNull(authority, "authority");
    var provider = com.venture.runtime.RuntimeEnvironment.get().contentProviders.get(authority);
    if (provider == null) throw new IllegalArgumentException("Unknown authority " + authority);
    return provider.call(authority, method, arg, extras);
  }
  // A picked document's grant already lasts across launches (HostDocuments keeps
  // its roots), so taking or releasing a persistable grant changes nothing.
  public void takePersistableUriPermission(android.net.Uri uri, int modeFlags) {
    java.util.Objects.requireNonNull(uri, "uri");
  }
  public void releasePersistableUriPermission(android.net.Uri uri, int modeFlags) {
    java.util.Objects.requireNonNull(uri, "uri");
  }
}
