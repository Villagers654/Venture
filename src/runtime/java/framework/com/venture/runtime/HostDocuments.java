package com.venture.runtime;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * The storage access framework's documents over host folders and files the
 * person picked (Venture's document picker, com.venture.runtime.DocumentPicker):
 * Android's external storage provider, whose authority apps expect, with each
 * picked folder a root. A document ID is "<root>:<path below it>", as
 * com.android.externalstorage.documents names them ("primary:Download/a.txt").
 * Only picked roots are reachable; the roots persist per package beside its
 * runtime permission grants, as a persistable URI grant lasts across launches.
 */
public final class HostDocuments extends ContentProvider {
  public static final String AUTHORITY = "com.android.externalstorage.documents";
  // AOSP DocumentsContract values.
  static final String DIRECTORY = "vnd.android.document/directory";
  static final String COLUMN_ID = "document_id", COLUMN_MIME = "mime_type", COLUMN_NAME = "_display_name",
      COLUMN_MODIFIED = "last_modified", COLUMN_FLAGS = "flags", COLUMN_SIZE = "_size";
  static final String[] COLUMNS = {COLUMN_ID, COLUMN_MIME, COLUMN_NAME, COLUMN_MODIFIED, COLUMN_FLAGS, COLUMN_SIZE};
  static final int FLAG_SUPPORTS_WRITE = 2, FLAG_SUPPORTS_DELETE = 4, FLAG_DIR_SUPPORTS_CREATE = 8,
      FLAG_SUPPORTS_RENAME = 64, FLAG_SUPPORTS_MOVE = 256;
  static final String EXTRA_URI = "uri", EXTRA_TARGET_URI = "android.content.extra.TARGET_URI",
      EXTRA_PARENT_URI = "parentUri", EXTRA_RESULT = "result";

  private static final Map<String, File> roots = new LinkedHashMap<>();
  private static boolean loaded;

  /** Installs the provider under its authority (once per process). */
  public static void install() {
    var env = RuntimeEnvironment.get();
    env.contentProviders.putIfAbsent(AUTHORITY, new HostDocuments());
  }

  /** At launch: the provider, when the app holds documents picked on an earlier launch. */
  public static synchronized void installIfGranted() {
    load();
    if (!roots.isEmpty()) install();
  }

  @Override public boolean onCreate() { return true; }

  // --- Roots -------------------------------------------------------------

  /**
   * The root ID for a picked host folder, granted and remembered. A root is a
   * removable storage volume, as an SD card's folder is on Android: its ID has a
   * volume UUID's form ("1A2B-3C4D", from the folder's path) and the guest sees
   * it at /storage/<id>, so apps that turn a tree URI into a path find it.
   */
  public static synchronized String grant(File folder) throws IOException {
    load();
    File canonical = folder.getCanonicalFile();
    for (var entry : roots.entrySet())
      if (entry.getValue().equals(canonical)) return entry.getKey();
    java.util.zip.CRC32 crc = new java.util.zip.CRC32();
    crc.update(canonical.getPath().getBytes(StandardCharsets.UTF_8));
    long value = crc.getValue();
    String id;
    do {
      id = String.format("%04X-%04X", (value >>> 16) & 0xffff, value & 0xffff);
      ++value;
    } while (roots.containsKey(id));
    roots.put(id, canonical);
    save();
    mount(id, canonical);
    install();
    return id;
  }

  /** The picked folders: volume ID to host folder. */
  public static synchronized Map<String, File> roots() {
    load();
    return new LinkedHashMap<>(roots);
  }

  private static void mount(String id, File folder) {
    try {
      if (!RuntimeEnvironment.host().mountDocumentRoot(id, folder.getPath()))
        android.util.Log.w("HostDocuments", "Cannot mount document root " + id);
    } catch (RuntimeException | LinkageError noHost) {
      // tests and hosts without a guest: documents still work through Java
    }
  }

  private static void load() {
    if (loaded) return;
    loaded = true;
    Path file = store();
    if (file == null || !Files.exists(file)) return;
    try {
      for (String line : Files.readAllLines(file, StandardCharsets.UTF_8)) {
        int tab = line.indexOf('\t');
        if (tab <= 0) continue;
        File folder = new File(line.substring(tab + 1));
        roots.put(line.substring(0, tab), folder);
        mount(line.substring(0, tab), folder);
      }
    } catch (IOException e) {
      android.util.Log.w("HostDocuments", "Cannot read document roots: " + e);
    }
  }

  private static void save() {
    Path file = store();
    if (file == null) return;
    try {
      Files.createDirectories(file.getParent());
      List<String> lines = new ArrayList<>();
      for (var entry : roots.entrySet()) lines.add(entry.getKey() + "\t" + entry.getValue().getPath());
      Path temp = Files.createTempFile(file.getParent(), "roots-", ".tmp");
      try {
        Files.write(temp, lines, StandardCharsets.UTF_8);
        Files.move(temp, file, StandardCopyOption.REPLACE_EXISTING, StandardCopyOption.ATOMIC_MOVE);
      } finally {
        Files.deleteIfExists(temp);
      }
    } catch (IOException e) {
      android.util.Log.w("HostDocuments", "Cannot save document roots: " + e);
    }
  }

  private static Path store() {
    RuntimeEnvironment env = RuntimeEnvironment.current();
    if (env == null) return null;
    File user = env.data.getParentFile();
    return (user != null ? user : env.data).toPath().resolve("document-roots")
        .resolve(env.manifest.info.packageName + ".txt");
  }

  // --- Document IDs and URIs ---------------------------------------------

  public static String documentId(String root, String relative) {
    return root + ":" + relative;
  }

  /** The host file a document ID names, inside its root. */
  static synchronized File file(String documentId) throws FileNotFoundException {
    load();
    int colon = documentId == null ? -1 : documentId.indexOf(':');
    if (colon <= 0) throw new FileNotFoundException("Unknown document " + documentId);
    File root = roots.get(documentId.substring(0, colon));
    if (root == null) throw new FileNotFoundException("Unknown document " + documentId);
    String relative = documentId.substring(colon + 1);
    File file = relative.isEmpty() ? root : new File(root, relative.replace('/', File.separatorChar));
    try {
      File canonical = file.getCanonicalFile();
      if (!canonical.equals(root) && !canonical.getPath().startsWith(root.getPath() + File.separator))
        throw new FileNotFoundException("Document outside its root " + documentId);
      return canonical;
    } catch (IOException e) {
      FileNotFoundException failure = new FileNotFoundException(e.getMessage());
      failure.initCause(e);
      throw failure;
    }
  }

  private static String child(String parentId, String name) {
    return parentId.endsWith(":") ? parentId + name : parentId + "/" + name;
  }

  public static Uri treeUri(String documentId) {
    return new Uri.Builder().scheme("content").authority(AUTHORITY).appendPath("tree").appendPath(documentId).build();
  }

  public static Uri documentUri(String documentId) {
    return new Uri.Builder().scheme("content").authority(AUTHORITY).appendPath("document").appendPath(documentId).build();
  }

  /** A document URI under the same tree as `base` when it has one. */
  static Uri documentUri(Uri base, String documentId) {
    List<String> segments = base.getPathSegments();
    if (segments.size() >= 2 && segments.get(0).equals("tree"))
      return new Uri.Builder().scheme("content").authority(AUTHORITY).appendPath("tree").appendPath(segments.get(1))
          .appendPath("document").appendPath(documentId).build();
    return documentUri(documentId);
  }

  /** The document a URI names: tree/<id>[/document/<id>[/children]] or document/<id>. */
  static String documentIdOf(Uri uri) throws FileNotFoundException {
    List<String> segments = uri.getPathSegments();
    if (segments.size() >= 4 && segments.get(0).equals("tree") && segments.get(2).equals("document"))
      return segments.get(3);
    if (segments.size() >= 2 && (segments.get(0).equals("tree") || segments.get(0).equals("document")))
      return segments.get(1);
    throw new FileNotFoundException("Not a document URI: " + uri);
  }

  // --- Provider ----------------------------------------------------------

  static String mimeType(File file) {
    if (file.isDirectory()) return DIRECTORY;
    String type = java.net.URLConnection.guessContentTypeFromName(file.getName());
    return type != null ? type : "application/octet-stream";
  }

  private static void addRow(MatrixCursor cursor, String[] projection, String documentId, File file) {
    boolean directory = file.isDirectory();
    Object[] row = new Object[projection.length];
    for (int i = 0; i < projection.length; ++i) {
      row[i] = switch (projection[i]) {
        case COLUMN_ID -> documentId;
        case COLUMN_MIME -> mimeType(file);
        case COLUMN_NAME -> file.getName();
        case COLUMN_MODIFIED -> file.lastModified();
        case COLUMN_FLAGS -> FLAG_SUPPORTS_DELETE | FLAG_SUPPORTS_RENAME | FLAG_SUPPORTS_MOVE
            | (directory ? FLAG_DIR_SUPPORTS_CREATE : FLAG_SUPPORTS_WRITE);
        case COLUMN_SIZE -> directory ? null : file.length();
        default -> null;
      };
    }
    cursor.addRow(row);
  }

  @Override
  public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
    String[] columns = projection != null ? projection : COLUMNS;
    MatrixCursor cursor = new MatrixCursor(columns);
    try {
      List<String> segments = uri.getPathSegments();
      String documentId = documentIdOf(uri);
      File file = file(documentId);
      if (!file.exists()) return cursor;
      if (segments.get(segments.size() - 1).equals("children") && segments.size() >= 5) {
        File[] children = file.listFiles();
        if (children != null) {
          java.util.Arrays.sort(children);
          for (File child : children) addRow(cursor, columns, child(documentId, child.getName()), child);
        }
      } else {
        addRow(cursor, columns, documentId, file);
      }
    } catch (FileNotFoundException unknown) {
      // an empty cursor, as for a document that does not exist
    }
    return cursor;
  }

  @Override
  public String getType(Uri uri) {
    try {
      File file = file(documentIdOf(uri));
      return file.exists() ? mimeType(file) : null;
    } catch (FileNotFoundException unknown) {
      return null;
    }
  }

  @Override
  public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
    File file = file(documentIdOf(uri));
    if (file.isDirectory()) throw new FileNotFoundException("Directory " + uri);
    return ParcelFileDescriptor.open(file, ParcelFileDescriptor.parseMode(mode));
  }

  @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException("insert"); }
  @Override public int update(Uri uri, ContentValues values, String selection, String[] args) { return 0; }
  @Override public int delete(Uri uri, String selection, String[] args) { return 0; }

  @Override
  public Bundle call(String authority, String method, String arg, Bundle extras) {
    try {
      return call(method, extras);
    } catch (IOException e) {
      throw new IllegalStateException(e.getMessage(), e);
    }
  }

  @Override
  public Bundle call(String method, String arg, Bundle extras) { return call(AUTHORITY, method, arg, extras); }

  private Bundle call(String method, Bundle extras) throws IOException {
    Uri uri = extras == null ? null : (Uri) extras.getParcelable(EXTRA_URI);
    if (uri == null) throw new IllegalArgumentException("Missing document URI for " + method);
    String documentId = documentIdOf(uri);
    File file = file(documentId);
    Bundle out = new Bundle();
    switch (method) {
      case "android:createDocument" -> {
        String mime = extras.getString(COLUMN_MIME);
        String name = safeName(extras.getString(COLUMN_NAME));
        File created = unique(file, name);
        boolean made = DIRECTORY.equals(mime) ? created.mkdir() : created.createNewFile();
        if (!made) throw new IOException("Cannot create " + name);
        out.putParcelable(EXTRA_URI, documentUri(uri, child(documentId, created.getName())));
      }
      case "android:renameDocument" -> {
        String name = safeName(extras.getString(COLUMN_NAME));
        File renamed = new File(file.getParentFile(), name);
        Files.move(file.toPath(), renamed.toPath());
        out.putParcelable(EXTRA_URI, documentUri(uri, child(parentId(documentId), name)));
      }
      case "android:deleteDocument", "android:removeDocument" -> deleteTree(file.toPath());
      case "android:moveDocument", "android:copyDocument" -> {
        Uri target = (Uri) extras.getParcelable(EXTRA_TARGET_URI);
        String targetId = documentIdOf(target);
        File destination = unique(file(targetId), file.getName());
        if (method.equals("android:moveDocument")) Files.move(file.toPath(), destination.toPath());
        else copyTree(file.toPath(), destination.toPath());
        out.putParcelable(EXTRA_URI, documentUri(uri, child(targetId, destination.getName())));
      }
      case "android:isChildDocument" -> {
        Uri target = (Uri) extras.getParcelable(EXTRA_TARGET_URI);
        File child = file(documentIdOf(target));
        out.putBoolean(EXTRA_RESULT, child.getPath().startsWith(file.getPath() + File.separator));
      }
      default -> throw new UnsupportedOperationException("Document method " + method);
    }
    return out;
  }

  private static String parentId(String documentId) {
    int slash = documentId.lastIndexOf('/');
    return slash >= 0 ? documentId.substring(0, slash) : documentId.substring(0, documentId.indexOf(':') + 1);
  }

  // A display name is one path component: Android's FileUtils.buildValidFatFilename
  // replaces the characters FAT and NTFS refuse.
  private static String safeName(String name) {
    if (name == null || name.isEmpty()) name = "untitled";
    StringBuilder out = new StringBuilder();
    for (char c : name.toCharArray())
      out.append(c < 0x20 || "\"*/:<>?\\|".indexOf(c) >= 0 ? '_' : c);
    String result = out.toString();
    return result.equals(".") || result.equals("..") ? "_" : result;
  }

  // As DocumentsProvider does for an existing name: "name (1).ext".
  private static File unique(File parent, String name) {
    File file = new File(parent, name);
    if (!file.exists()) return file;
    int dot = name.lastIndexOf('.');
    String base = dot > 0 ? name.substring(0, dot) : name, extension = dot > 0 ? name.substring(dot) : "";
    for (int i = 1; ; ++i) {
      file = new File(parent, base + " (" + i + ")" + extension);
      if (!file.exists()) return file;
    }
  }

  private static void deleteTree(Path path) throws IOException {
    if (Files.isDirectory(path, java.nio.file.LinkOption.NOFOLLOW_LINKS))
      try (var children = Files.list(path)) {
        for (Path child : (Iterable<Path>) children::iterator) deleteTree(child);
      }
    Files.delete(path);
  }

  private static void copyTree(Path from, Path to) throws IOException {
    if (Files.isDirectory(from, java.nio.file.LinkOption.NOFOLLOW_LINKS)) {
      Files.createDirectory(to);
      try (var children = Files.list(from)) {
        for (Path child : (Iterable<Path>) children::iterator) copyTree(child, to.resolve(child.getFileName()));
      }
    } else {
      Files.copy(from, to);
    }
  }
}
