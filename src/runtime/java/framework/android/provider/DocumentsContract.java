package android.provider;

import android.content.ContentResolver;
import android.content.Context;
import android.net.Uri;
import android.os.Bundle;
import java.io.FileNotFoundException;
import java.util.List;

/**
 * AOSP android.provider.DocumentsContract: document and tree URIs and the
 * document operations, which a provider answers through ContentResolver.call.
 * Picked folders are served by com.venture.runtime.HostDocuments. The URI
 * shapes are AOSP's: content://authority/tree/<id>/document/<id>/children and
 * the like. A 2D process keeps AOSP's own class from layoutlib, which reaches
 * the same provider the same way.
 */
public final class DocumentsContract {
  private DocumentsContract() {}

  public static final String EXTRA_INITIAL_URI = "android.provider.extra.INITIAL_URI";
  public static final String EXTRA_LOADING = "loading", EXTRA_INFO = "info", EXTRA_ERROR = "error";
  public static final String PROVIDER_INTERFACE = "android.content.action.DOCUMENTS_PROVIDER";
  public static final String METHOD_CREATE_DOCUMENT = "android:createDocument";
  public static final String METHOD_RENAME_DOCUMENT = "android:renameDocument";
  public static final String METHOD_DELETE_DOCUMENT = "android:deleteDocument";
  public static final String METHOD_COPY_DOCUMENT = "android:copyDocument";
  public static final String METHOD_MOVE_DOCUMENT = "android:moveDocument";
  public static final String METHOD_REMOVE_DOCUMENT = "android:removeDocument";
  public static final String METHOD_IS_CHILD_DOCUMENT = "android:isChildDocument";
  public static final String EXTRA_URI = "uri", EXTRA_PARENT_URI = "parentUri", EXTRA_RESULT = "result";
  public static final String EXTRA_TARGET_URI = "android.content.extra.TARGET_URI";
  private static final String PATH_ROOT = "root", PATH_DOCUMENT = "document", PATH_CHILDREN = "children",
      PATH_TREE = "tree", PATH_RECENT = "recent", PATH_SEARCH = "search";
  private static final String PARAM_QUERY = "query";

  public static final class Document {
    private Document() {}
    public static final String COLUMN_DOCUMENT_ID = "document_id", COLUMN_MIME_TYPE = "mime_type",
        COLUMN_DISPLAY_NAME = "_display_name", COLUMN_SUMMARY = "summary",
        COLUMN_LAST_MODIFIED = "last_modified", COLUMN_ICON = "icon", COLUMN_FLAGS = "flags",
        COLUMN_SIZE = "_size";
    public static final String MIME_TYPE_DIR = "vnd.android.document/directory";
    public static final int FLAG_SUPPORTS_THUMBNAIL = 1, FLAG_SUPPORTS_WRITE = 1 << 1,
        FLAG_SUPPORTS_DELETE = 1 << 2, FLAG_DIR_SUPPORTS_CREATE = 1 << 3, FLAG_DIR_PREFERS_GRID = 1 << 4,
        FLAG_DIR_PREFERS_LAST_MODIFIED = 1 << 5, FLAG_SUPPORTS_RENAME = 1 << 6, FLAG_SUPPORTS_COPY = 1 << 7,
        FLAG_SUPPORTS_MOVE = 1 << 8, FLAG_VIRTUAL_DOCUMENT = 1 << 9, FLAG_SUPPORTS_REMOVE = 1 << 10,
        FLAG_SUPPORTS_SETTINGS = 1 << 11, FLAG_WEB_LINKABLE = 1 << 12, FLAG_PARTIAL = 1 << 13,
        FLAG_SUPPORTS_METADATA = 1 << 14, FLAG_DIR_BLOCKS_OPEN_DOCUMENT_TREE = 1 << 15;
  }

  public static final class Root {
    private Root() {}
    public static final String COLUMN_ROOT_ID = "root_id", COLUMN_FLAGS = "flags", COLUMN_ICON = "icon",
        COLUMN_TITLE = "title", COLUMN_SUMMARY = "summary", COLUMN_DOCUMENT_ID = "document_id",
        COLUMN_AVAILABLE_BYTES = "available_bytes", COLUMN_CAPACITY_BYTES = "capacity_bytes",
        COLUMN_MIME_TYPES = "mime_types", COLUMN_QUERY_ARGS = "query_args";
    public static final String MIME_TYPE_ITEM = "vnd.android.document/root";
    public static final int FLAG_SUPPORTS_CREATE = 1, FLAG_LOCAL_ONLY = 1 << 1, FLAG_SUPPORTS_RECENTS = 1 << 2,
        FLAG_SUPPORTS_SEARCH = 1 << 3, FLAG_SUPPORTS_IS_CHILD = 1 << 4, FLAG_SUPPORTS_EJECT = 1 << 5;
  }

  private static Uri.Builder base(String authority) {
    return new Uri.Builder().scheme(ContentResolver.SCHEME_CONTENT).authority(authority);
  }

  public static Uri buildRootsUri(String authority) { return base(authority).appendPath(PATH_ROOT).build(); }
  public static Uri buildRootUri(String authority, String rootId) {
    return base(authority).appendPath(PATH_ROOT).appendPath(rootId).build();
  }
  public static Uri buildRecentDocumentsUri(String authority, String rootId) {
    return base(authority).appendPath(PATH_ROOT).appendPath(rootId).appendPath(PATH_RECENT).build();
  }
  public static Uri buildSearchDocumentsUri(String authority, String rootId, String query) {
    return base(authority).appendPath(PATH_ROOT).appendPath(rootId).appendPath(PATH_SEARCH)
        .appendQueryParameter(PARAM_QUERY, query).build();
  }
  public static Uri buildTreeDocumentUri(String authority, String documentId) {
    return base(authority).appendPath(PATH_TREE).appendPath(documentId).build();
  }
  public static Uri buildDocumentUri(String authority, String documentId) {
    return base(authority).appendPath(PATH_DOCUMENT).appendPath(documentId).build();
  }
  public static Uri buildDocumentUriUsingTree(Uri treeUri, String documentId) {
    return base(treeUri.getAuthority()).appendPath(PATH_TREE).appendPath(getTreeDocumentId(treeUri))
        .appendPath(PATH_DOCUMENT).appendPath(documentId).build();
  }
  public static Uri buildDocumentUriMaybeUsingTree(Uri baseUri, String documentId) {
    return isTreeUri(baseUri) ? buildDocumentUriUsingTree(baseUri, documentId)
                              : buildDocumentUri(baseUri.getAuthority(), documentId);
  }
  public static Uri buildChildDocumentsUri(String authority, String parentDocumentId) {
    return base(authority).appendPath(PATH_DOCUMENT).appendPath(parentDocumentId).appendPath(PATH_CHILDREN).build();
  }
  public static Uri buildChildDocumentsUriUsingTree(Uri treeUri, String parentDocumentId) {
    return base(treeUri.getAuthority()).appendPath(PATH_TREE).appendPath(getTreeDocumentId(treeUri))
        .appendPath(PATH_DOCUMENT).appendPath(parentDocumentId).appendPath(PATH_CHILDREN).build();
  }

  public static boolean isContentUri(Uri uri) {
    return uri != null && ContentResolver.SCHEME_CONTENT.equals(uri.getScheme());
  }
  public static boolean isDocumentUri(Context context, Uri uri) {
    if (!isContentUri(uri)) return false;
    List<String> path = uri.getPathSegments();
    if (path.size() == 2) return PATH_DOCUMENT.equals(path.get(0));
    return path.size() == 4 && PATH_TREE.equals(path.get(0)) && PATH_DOCUMENT.equals(path.get(2));
  }
  public static boolean isRootsUri(Context context, Uri uri) {
    return isContentUri(uri) && uri.getPathSegments().size() == 1 && PATH_ROOT.equals(uri.getPathSegments().get(0));
  }
  public static boolean isRootUri(Context context, Uri uri) {
    return isContentUri(uri) && uri.getPathSegments().size() == 2 && PATH_ROOT.equals(uri.getPathSegments().get(0));
  }
  public static boolean isTreeUri(Uri uri) {
    List<String> path = uri.getPathSegments();
    return path.size() >= 2 && PATH_TREE.equals(path.get(0));
  }

  public static String getRootId(Uri rootUri) {
    List<String> path = rootUri.getPathSegments();
    if (path.size() >= 2 && PATH_ROOT.equals(path.get(0))) return path.get(1);
    throw new IllegalArgumentException("Invalid URI: " + rootUri);
  }
  public static String getDocumentId(Uri documentUri) {
    List<String> path = documentUri.getPathSegments();
    if (path.size() >= 2 && PATH_DOCUMENT.equals(path.get(0))) return path.get(1);
    if (path.size() >= 4 && PATH_TREE.equals(path.get(0)) && PATH_DOCUMENT.equals(path.get(2))) return path.get(3);
    throw new IllegalArgumentException("Invalid URI: " + documentUri);
  }
  public static String getTreeDocumentId(Uri documentUri) {
    List<String> path = documentUri.getPathSegments();
    if (path.size() >= 2 && PATH_TREE.equals(path.get(0))) return path.get(1);
    throw new IllegalArgumentException("Invalid URI: " + documentUri);
  }
  public static String getSearchDocumentsQuery(Uri searchDocumentsUri) {
    return searchDocumentsUri.getQueryParameter(PARAM_QUERY);
  }

  public static Uri createDocument(ContentResolver content, Uri parentDocumentUri, String mimeType,
      String displayName) throws FileNotFoundException {
    Bundle in = new Bundle();
    in.putParcelable(EXTRA_URI, parentDocumentUri);
    in.putString(Document.COLUMN_MIME_TYPE, mimeType);
    in.putString(Document.COLUMN_DISPLAY_NAME, displayName);
    Bundle out = call(content, parentDocumentUri, METHOD_CREATE_DOCUMENT, in);
    return out == null ? null : (Uri) out.getParcelable(EXTRA_URI);
  }
  public static boolean isChildDocument(ContentResolver content, Uri parentDocumentUri, Uri childDocumentUri)
      throws FileNotFoundException {
    Bundle in = new Bundle();
    in.putParcelable(EXTRA_URI, parentDocumentUri);
    in.putParcelable(EXTRA_TARGET_URI, childDocumentUri);
    Bundle out = call(content, parentDocumentUri, METHOD_IS_CHILD_DOCUMENT, in);
    return out != null && out.getBoolean(EXTRA_RESULT);
  }
  public static Uri renameDocument(ContentResolver content, Uri documentUri, String displayName)
      throws FileNotFoundException {
    Bundle in = new Bundle();
    in.putParcelable(EXTRA_URI, documentUri);
    in.putString(Document.COLUMN_DISPLAY_NAME, displayName);
    Bundle out = call(content, documentUri, METHOD_RENAME_DOCUMENT, in);
    if (out == null) return null;
    Uri renamed = (Uri) out.getParcelable(EXTRA_URI);
    return renamed != null ? renamed : documentUri;
  }
  public static boolean deleteDocument(ContentResolver content, Uri documentUri) throws FileNotFoundException {
    Bundle in = new Bundle();
    in.putParcelable(EXTRA_URI, documentUri);
    return call(content, documentUri, METHOD_DELETE_DOCUMENT, in) != null;
  }
  public static boolean removeDocument(ContentResolver content, Uri documentUri, Uri parentDocumentUri)
      throws FileNotFoundException {
    Bundle in = new Bundle();
    in.putParcelable(EXTRA_URI, documentUri);
    in.putParcelable(EXTRA_PARENT_URI, parentDocumentUri);
    return call(content, documentUri, METHOD_REMOVE_DOCUMENT, in) != null;
  }
  public static Uri copyDocument(ContentResolver content, Uri sourceDocumentUri, Uri targetParentDocumentUri)
      throws FileNotFoundException {
    Bundle in = new Bundle();
    in.putParcelable(EXTRA_URI, sourceDocumentUri);
    in.putParcelable(EXTRA_TARGET_URI, targetParentDocumentUri);
    Bundle out = call(content, sourceDocumentUri, METHOD_COPY_DOCUMENT, in);
    return out == null ? null : (Uri) out.getParcelable(EXTRA_URI);
  }
  public static Uri moveDocument(ContentResolver content, Uri sourceDocumentUri, Uri sourceParentDocumentUri,
      Uri targetParentDocumentUri) throws FileNotFoundException {
    Bundle in = new Bundle();
    in.putParcelable(EXTRA_URI, sourceDocumentUri);
    in.putParcelable(EXTRA_PARENT_URI, sourceParentDocumentUri);
    in.putParcelable(EXTRA_TARGET_URI, targetParentDocumentUri);
    Bundle out = call(content, sourceDocumentUri, METHOD_MOVE_DOCUMENT, in);
    return out == null ? null : (Uri) out.getParcelable(EXTRA_URI);
  }

  // As AOSP for apps targeting API 29 and later: a missing document throws
  // FileNotFoundException, any other provider failure is logged and the call
  // answers null or false.
  private static Bundle call(ContentResolver content, Uri uri, String method, Bundle in)
      throws FileNotFoundException {
    try {
      Bundle out = content.call(uri.getAuthority(), method, null, in);
      return out != null ? out : new Bundle();
    } catch (RuntimeException failure) {
      if (failure.getCause() instanceof FileNotFoundException missing) throw missing;
      android.util.Log.w("DocumentsContract", "Failed " + method + " on " + uri + ": " + failure);
      return null;
    }
  }
}
