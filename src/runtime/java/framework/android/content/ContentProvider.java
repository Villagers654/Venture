package android.content;
public abstract class ContentProvider {
  private Context context;
  private String readPermission, writePermission;
  public abstract boolean onCreate();
  public abstract String getType(android.net.Uri uri);
  public final Context getContext() {
    return context;
  }
  public void attachInfo(Context c, android.content.pm.ProviderInfo info) {
    // As on Android, a provider attaches and creates once.
    if (context != null) return;
    context = c;
    if (info != null) {
      readPermission = info.readPermission;
      writePermission = info.writePermission;
    }
    onCreate();
  }
  /**
   * The package of the app calling this provider. Every caller runs in the game's own
   * process, which on Android names the game's package.
   */
  public final String getCallingPackage() {
    return context == null ? null : context.getPackageName();
  }
  public final String getCallingPackageUnchecked() {
    return getCallingPackage();
  }
  public final String getCallingAttributionTag() {
    return null;
  }
  public final String getReadPermission() {
    return readPermission;
  }
  public final String getWritePermission() {
    return writePermission;
  }
  protected final void setReadPermission(String permission) {
    readPermission = permission;
  }
  protected final void setWritePermission(String permission) {
    writePermission = permission;
  }
  public android.database.Cursor query(android.net.Uri uri, String[] projection, String selection,
      String[] selectionArgs, String sortOrder) {
    return null;
  }
  public android.database.Cursor query(android.net.Uri uri, String[] projection, String selection,
      String[] selectionArgs, String sortOrder, android.os.CancellationSignal cancellationSignal) {
    return query(uri, projection, selection, selectionArgs, sortOrder);
  }
  public android.database.Cursor query(android.net.Uri uri, String[] projection, android.os.Bundle queryArgs,
      android.os.CancellationSignal cancellationSignal) {
    queryArgs = queryArgs != null ? queryArgs : android.os.Bundle.EMPTY;
    String sortOrder = queryArgs.getString(ContentResolver.QUERY_ARG_SQL_SORT_ORDER);
    return query(uri, projection, queryArgs.getString(ContentResolver.QUERY_ARG_SQL_SELECTION),
        queryArgs.getStringArray(ContentResolver.QUERY_ARG_SQL_SELECTION_ARGS), sortOrder, cancellationSignal);
  }
  public android.net.Uri insert(android.net.Uri uri, ContentValues values) {
    return null;
  }
  public android.net.Uri insert(android.net.Uri uri, ContentValues values, android.os.Bundle extras) {
    return insert(uri, values);
  }
  public int bulkInsert(android.net.Uri uri, ContentValues[] values) {
    int inserted = 0;
    for (ContentValues value : values)
      if (insert(uri, value) != null) inserted++;
    return inserted;
  }
  public int update(android.net.Uri uri, ContentValues values, String selection, String[] selectionArgs) {
    return 0;
  }
  public int update(android.net.Uri uri, ContentValues values, android.os.Bundle extras) {
    extras = extras != null ? extras : android.os.Bundle.EMPTY;
    return update(uri, values, extras.getString(ContentResolver.QUERY_ARG_SQL_SELECTION),
        extras.getStringArray(ContentResolver.QUERY_ARG_SQL_SELECTION_ARGS));
  }
  public int delete(android.net.Uri uri, String selection, String[] selectionArgs) {
    return 0;
  }
  public int delete(android.net.Uri uri, android.os.Bundle extras) {
    extras = extras != null ? extras : android.os.Bundle.EMPTY;
    return delete(uri, extras.getString(ContentResolver.QUERY_ARG_SQL_SELECTION),
        extras.getStringArray(ContentResolver.QUERY_ARG_SQL_SELECTION_ARGS));
  }
  public android.os.Bundle call(String method, String arg, android.os.Bundle extras) {
    return null;
  }
  public android.os.Bundle call(String authority, String method, String arg, android.os.Bundle extras) {
    return call(method, arg, extras);
  }
  public android.os.ParcelFileDescriptor openFile(android.net.Uri uri, String mode)
      throws java.io.FileNotFoundException {
    throw new java.io.FileNotFoundException("No files supported by provider at " + uri);
  }
  public android.os.ParcelFileDescriptor openFile(android.net.Uri uri, String mode,
      android.os.CancellationSignal signal) throws java.io.FileNotFoundException {
    return openFile(uri, mode);
  }
  public void onLowMemory() {}
  public void onTrimMemory(int level) {}
  public void onConfigurationChanged(android.content.res.Configuration newConfig) {}
  public void shutdown() {}
}
