package android.app;

import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import com.venture.runtime.RuntimeEnvironment;
import java.io.File;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.Reader;
import java.io.Writer;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Properties;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.function.Consumer;

/**
 * AOSP android.app.DownloadManager. On a Quest the system DownloadProvider fetches HTTP(S)
 * URIs in the background, keeps one record per download across app restarts and broadcasts
 * ACTION_DOWNLOAD_COMPLETE to the app. Here the hosted process downloads with the host JDK's
 * HTTP stack, keeps the records in its data directory and resumes unfinished downloads when the
 * service is next created, as the provider does after a reboot. Notifications have no PC shade.
 */
public class DownloadManager {
  public static final String COLUMN_ID = "_id", COLUMN_TITLE = "title", COLUMN_DESCRIPTION = "description",
      COLUMN_URI = "uri", COLUMN_MEDIA_TYPE = "media_type", COLUMN_TOTAL_SIZE_BYTES = "total_size",
      COLUMN_LOCAL_URI = "local_uri", COLUMN_LOCAL_FILENAME = "local_filename", COLUMN_STATUS = "status",
      COLUMN_REASON = "reason", COLUMN_BYTES_DOWNLOADED_SO_FAR = "bytes_so_far",
      COLUMN_LAST_MODIFIED_TIMESTAMP = "last_modified_timestamp", COLUMN_MEDIAPROVIDER_URI = "mediaprovider_uri";
  public static final int STATUS_PENDING = 1, STATUS_RUNNING = 2, STATUS_PAUSED = 4, STATUS_SUCCESSFUL = 8,
      STATUS_FAILED = 16;
  public static final int ERROR_UNKNOWN = 1000, ERROR_FILE_ERROR = 1001, ERROR_UNHANDLED_HTTP_CODE = 1002,
      ERROR_HTTP_DATA_ERROR = 1004, ERROR_TOO_MANY_REDIRECTS = 1005, ERROR_INSUFFICIENT_SPACE = 1006,
      ERROR_DEVICE_NOT_FOUND = 1007, ERROR_CANNOT_RESUME = 1008, ERROR_FILE_ALREADY_EXISTS = 1009;
  public static final int PAUSED_WAITING_TO_RETRY = 1, PAUSED_WAITING_FOR_NETWORK = 2,
      PAUSED_QUEUED_FOR_WIFI = 3, PAUSED_UNKNOWN = 4;
  public static final String ACTION_DOWNLOAD_COMPLETE = "android.intent.action.DOWNLOAD_COMPLETE",
      ACTION_NOTIFICATION_CLICKED = "android.intent.action.DOWNLOAD_NOTIFICATION_CLICKED",
      ACTION_VIEW_DOWNLOADS = "android.intent.action.VIEW_DOWNLOADS",
      INTENT_EXTRAS_SORT_BY_SIZE = "android.app.DownloadManager.extra_sortBySize",
      EXTRA_DOWNLOAD_ID = "extra_download_id",
      EXTRA_NOTIFICATION_CLICK_DOWNLOAD_IDS = "extra_click_download_ids";

  private static final String[] COLUMNS = {COLUMN_ID, COLUMN_TITLE, COLUMN_DESCRIPTION, COLUMN_URI,
      COLUMN_MEDIA_TYPE, COLUMN_TOTAL_SIZE_BYTES, COLUMN_LOCAL_URI, COLUMN_LOCAL_FILENAME, COLUMN_STATUS,
      COLUMN_REASON, COLUMN_BYTES_DOWNLOADED_SO_FAR, COLUMN_LAST_MODIFIED_TIMESTAMP, COLUMN_MEDIAPROVIDER_URI};
  private static final int MAX_REDIRECTS = 7;  // DownloadThread.MAX_REDIRECTS
  private static final long PROGRESS_SAVE_MS = 1500;

  public static class Request {
    public static final int NETWORK_MOBILE = 1, NETWORK_WIFI = 2;
    public static final int VISIBILITY_VISIBLE = 0, VISIBILITY_VISIBLE_NOTIFY_COMPLETED = 1, VISIBILITY_HIDDEN = 2,
        VISIBILITY_VISIBLE_NOTIFY_ONLY_COMPLETION = 3;
    final Uri uri;
    Uri destination;
    CharSequence title, description;
    String mimeType;
    final List<String[]> headers = new ArrayList<>();
    public Request(Uri uri) {
      if (uri == null) throw new NullPointerException();
      String scheme = uri.getScheme();
      if (scheme == null || !(scheme.equals("http") || scheme.equals("https")))
        throw new IllegalArgumentException("Can only download HTTP/HTTPS URIs: " + uri);
      this.uri = uri;
    }
    public Request setDestinationUri(Uri uri) { destination = uri; return this; }
    public Request setDestinationInExternalFilesDir(Context context, String dirType, String subPath) {
      File base = context.getExternalFilesDir(dirType);
      if (base == null) throw new IllegalStateException("Failed to get external storage files directory");
      destination = Uri.fromFile(new File(base, subPath));
      return this;
    }
    public Request setDestinationInExternalPublicDir(String dirType, String subPath) {
      File base = Environment.getExternalStoragePublicDirectory(dirType);
      if (!base.isDirectory() && !base.mkdirs())
        throw new IllegalStateException("Unable to create directory: " + base.getAbsolutePath());
      destination = Uri.fromFile(new File(base, subPath));
      return this;
    }
    public void allowScanningByMediaScanner() {}
    public Request addRequestHeader(String header, String value) {
      if (header == null) throw new NullPointerException("header cannot be null");
      if (header.contains(":")) throw new IllegalArgumentException("header may not contain ':'");
      headers.add(new String[] {header, value == null ? "" : value});
      return this;
    }
    public Request setTitle(CharSequence title) { this.title = title; return this; }
    public Request setDescription(CharSequence description) { this.description = description; return this; }
    public Request setMimeType(String mimeType) { this.mimeType = mimeType; return this; }
    public Request setShowRunningNotification(boolean show) { return this; }
    public Request setNotificationVisibility(int visibility) { return this; }
    public Request setAllowedNetworkTypes(int flags) { return this; }
    public Request setAllowedOverRoaming(boolean allowed) { return this; }
    public Request setAllowedOverMetered(boolean allow) { return this; }
    public Request setRequiresCharging(boolean requiresCharging) { return this; }
    public Request setRequiresDeviceIdle(boolean requiresDeviceIdle) { return this; }
    public Request setVisibleInDownloadsUi(boolean isVisible) { return this; }
  }

  public static class Query {
    public static final int ORDER_ASCENDING = 1, ORDER_DESCENDING = 2;
    long[] ids;
    Integer statusFlags;
    String orderColumn = COLUMN_LAST_MODIFIED_TIMESTAMP;
    int orderDirection = ORDER_DESCENDING;
    public Query setFilterById(long... ids) { this.ids = ids; return this; }
    public Query setFilterByStatus(int flags) { statusFlags = flags; return this; }
    public Query setOnlyIncludeVisibleInDownloadsUi(boolean value) { return this; }
    public Query orderBy(String column, int direction) {
      if (direction != ORDER_ASCENDING && direction != ORDER_DESCENDING)
        throw new IllegalArgumentException("Invalid direction: " + direction);
      if (!column.equals(COLUMN_LAST_MODIFIED_TIMESTAMP) && !column.equals(COLUMN_TOTAL_SIZE_BYTES))
        throw new IllegalArgumentException("Cannot order by " + column);
      orderColumn = column;
      orderDirection = direction;
      return this;
    }
  }

  private static final class Record {
    long id;
    String uri, destination, title, description, mimeType, headers = "";
    int status = STATUS_PENDING, reason;
    long total = -1, done, modified;
    transient Future<?> task;
    transient volatile boolean removed;
    transient long saved;
  }

  private static DownloadManager instance;
  private final File store;
  private final File defaultDirectory;
  private final Consumer<Intent> broadcast;
  private final String packageName;
  private final Map<Long, Record> records = new LinkedHashMap<>();
  private final ExecutorService workers = Executors.newFixedThreadPool(3, runnable -> {
    Thread thread = new Thread(runnable, "DownloadManager");
    thread.setDaemon(true);
    return thread;
  });
  private long nextId = 1;

  /** The process's download service (Context.getSystemService(DOWNLOAD_SERVICE)). */
  public static synchronized DownloadManager ventureLocal(Context context) {
    if (instance == null) {
      Context app = context.getApplicationContext() != null ? context.getApplicationContext() : context;
      instance = new DownloadManager(new File(context.getNoBackupFilesDir(), ".venture-downloads"),
          new File(context.getExternalFilesDir(null), Environment.DIRECTORY_DOWNLOADS), app::sendBroadcast,
          context.getPackageName());
    }
    return instance;
  }
  DownloadManager(File store, File defaultDirectory, Consumer<Intent> broadcast, String packageName) {
    this.store = store;
    this.defaultDirectory = defaultDirectory;
    this.broadcast = broadcast;
    this.packageName = packageName;
    load();
    List<Record> unfinished = new ArrayList<>();
    synchronized (this) {
      for (Record r : records.values())
        if (r.status == STATUS_PENDING || r.status == STATUS_RUNNING || r.status == STATUS_PAUSED) unfinished.add(r);
    }
    for (Record r : unfinished) start(r);
  }

  public long enqueue(Request request) {
    Record r = new Record();
    r.uri = request.uri.toString();
    r.destination = request.destination != null ? request.destination.toString() : null;
    r.title = request.title != null ? request.title.toString() : null;
    r.description = request.description != null ? request.description.toString() : null;
    r.mimeType = request.mimeType;
    StringBuilder headers = new StringBuilder();
    for (String[] h : request.headers) headers.append(h[0]).append(':').append(h[1]).append('\n');
    r.headers = headers.toString();
    synchronized (this) {
      r.id = nextId++;
      r.modified = System.currentTimeMillis();
      if (r.destination == null) {
        String name = request.uri.getLastPathSegment();
        if (name == null || name.isEmpty()) name = "downloadfile";
        r.destination = Uri.fromFile(new File(defaultDirectory, r.id + "-" + name)).toString();
      }
      records.put(r.id, r);
      save();
    }
    start(r);
    return r.id;
  }

  public int remove(long... ids) {
    if (ids == null || ids.length == 0) throw new IllegalArgumentException("input param 'ids' can't be null");
    int count = 0;
    for (long id : ids) {
      Record r;
      synchronized (this) {
        r = records.remove(id);
        if (r == null) continue;
        r.removed = true;
        if (r.task != null) r.task.cancel(true);
        save();
      }
      count++;
      // DownloadProvider deletes the file of a removed download, finished or not.
      File file = file(r);
      if (file != null) {
        file.delete();
        partial(file).delete();
      }
    }
    return count;
  }

  public Cursor query(Query query) {
    List<Record> rows = new ArrayList<>();
    synchronized (this) {
      for (Record r : records.values()) {
        if (query != null && query.ids != null) {
          boolean match = false;
          for (long id : query.ids) match |= id == r.id;
          if (!match) continue;
        }
        if (query != null && query.statusFlags != null && (query.statusFlags & r.status) == 0) continue;
        rows.add(copy(r));
      }
    }
    boolean bySize = query != null && query.orderColumn.equals(COLUMN_TOTAL_SIZE_BYTES);
    boolean ascending = query != null && query.orderDirection == Query.ORDER_ASCENDING;
    rows.sort((a, b) -> {
      int order = bySize ? Long.compare(a.total, b.total) : Long.compare(a.modified, b.modified);
      return ascending ? order : -order;
    });
    MatrixCursor cursor = new MatrixCursor(COLUMNS, rows.size());
    for (Record r : rows) {
      boolean done = r.status == STATUS_SUCCESSFUL;
      File file = file(r);
      cursor.addRow(new Object[] {r.id, r.title != null ? r.title : "", r.description != null ? r.description : "",
          r.uri, r.mimeType, r.total, r.destination, file != null ? file.getPath() : null, r.status,
          r.reason, r.done, r.modified, null});
    }
    return cursor;
  }

  public Uri getUriForDownloadedFile(long id) {
    Record r = record(id);
    return r != null && r.status == STATUS_SUCCESSFUL ? Uri.parse(r.destination) : null;
  }
  public String getMimeTypeForDownloadedFile(long id) {
    Record r = record(id);
    return r == null ? null : r.mimeType;
  }
  public ParcelFileDescriptor openDownloadedFile(long id) throws FileNotFoundException {
    Record r = record(id);
    File file = r != null && r.status == STATUS_SUCCESSFUL ? file(r) : null;
    if (file == null) throw new FileNotFoundException("No file found with id " + id);
    return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
  }
  public static Long getMaxBytesOverMobile(Context context) { return null; }
  public static Long getRecommendedMaxBytesOverMobile(Context context) { return null; }

  private synchronized Record record(long id) {
    Record r = records.get(id);
    return r == null ? null : copy(r);
  }
  private static Record copy(Record r) {
    Record c = new Record();
    c.id = r.id; c.uri = r.uri; c.destination = r.destination; c.title = r.title; c.description = r.description;
    c.mimeType = r.mimeType; c.headers = r.headers; c.status = r.status; c.reason = r.reason; c.total = r.total;
    c.done = r.done; c.modified = r.modified;
    return c;
  }

  /** The host file a download is written to. */
  File ventureFile(long id) {
    Record r = record(id);
    return r == null ? null : file(r);
  }
  /** The host file of a record's file:// destination (an app path, or an Android path under its mounts). */
  private static File file(Record r) {
    if (r.destination == null) return null;
    Uri uri = Uri.parse(r.destination);
    if (uri.getScheme() != null && !uri.getScheme().equals("file")) return null;
    String path = uri.getPath();
    if (path == null) return null;
    // Uri.fromFile of a Windows host path: "/C:/..." or "C:\...".
    if (path.length() >= 3 && path.charAt(0) == '/' && path.charAt(2) == ':') path = path.substring(1);
    return new File(RuntimeEnvironment.toHostPath(path));
  }
  private static File partial(File file) { return new File(file.getPath() + ".venture-partial"); }

  private void start(Record r) {
    synchronized (this) {
      if (r.removed) return;
      r.task = workers.submit(() -> run(r));
    }
  }
  private void run(Record r) {
    File file = file(r);
    if (file == null) {
      finish(r, STATUS_FAILED, ERROR_FILE_ERROR);
      return;
    }
    File temp = partial(file);
    try {
      File parent = file.getAbsoluteFile().getParentFile();
      if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
        finish(r, STATUS_FAILED, ERROR_FILE_ERROR);
        return;
      }
      URL url = new URL(r.uri);
      long offset = temp.isFile() ? temp.length() : 0;
      HttpURLConnection connection = null;
      for (int redirects = 0;; redirects++) {
        if (redirects > MAX_REDIRECTS) {
          finish(r, STATUS_FAILED, ERROR_TOO_MANY_REDIRECTS);
          return;
        }
        connection = (HttpURLConnection) url.openConnection();
        connection.setInstanceFollowRedirects(false);
        connection.setConnectTimeout(20000);
        connection.setReadTimeout(20000);
        for (String line : r.headers.split("\n")) {
          int colon = line.indexOf(':');
          if (colon > 0) connection.addRequestProperty(line.substring(0, colon), line.substring(colon + 1));
        }
        if (offset > 0) connection.setRequestProperty("Range", "bytes=" + offset + "-");
        int code = connection.getResponseCode();
        if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
          String location = connection.getHeaderField("Location");
          connection.disconnect();
          if (location == null) {
            finish(r, STATUS_FAILED, ERROR_UNHANDLED_HTTP_CODE);
            return;
          }
          url = new URL(url, location);
          continue;
        }
        if (code == 416 && offset > 0) {  // the partial file is already complete or stale
          connection.disconnect();
          temp.delete();
          offset = 0;
          continue;
        }
        if (code != 200 && code != 206) {
          connection.disconnect();
          // DownloadThread: 4xx/5xx codes are the failure reason; others are unhandled.
          finish(r, STATUS_FAILED, code >= 400 && code < 600 ? code : ERROR_UNHANDLED_HTTP_CODE);
          return;
        }
        if (code == 200) offset = 0;
        break;
      }
      long length = connection.getContentLengthLong();
      synchronized (this) {
        if (r.removed) { connection.disconnect(); return; }
        r.status = STATUS_RUNNING;
        r.reason = 0;
        r.done = offset;
        r.total = length >= 0 ? offset + length : -1;
        if (r.mimeType == null) r.mimeType = connection.getContentType();
        r.modified = System.currentTimeMillis();
        save();
      }
      try (InputStream in = connection.getInputStream(); OutputStream out = new FileOutputStream(temp, offset > 0)) {
        byte[] buffer = new byte[64 * 1024];
        for (int n; (n = in.read(buffer)) >= 0;) {
          if (r.removed || Thread.currentThread().isInterrupted()) return;
          out.write(buffer, 0, n);
          synchronized (this) {
            r.done += n;
            long now = System.currentTimeMillis();
            r.modified = now;
            if (now - r.saved >= PROGRESS_SAVE_MS) { r.saved = now; save(); }
          }
        }
      } finally {
        connection.disconnect();
      }
      synchronized (this) {
        if (r.removed) return;
        if (r.total >= 0 && r.done != r.total) {
          // A short body: resume from the partial file next time, as after a dropped connection.
          r.status = STATUS_PAUSED;
          r.reason = PAUSED_WAITING_TO_RETRY;
          save();
          return;
        }
      }
      Files.move(temp.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING);
      finish(r, STATUS_SUCCESSFUL, 0);
    } catch (IOException error) {
      if (r.removed) return;
      android.util.Log.w("DownloadManager", "Download " + r.id + " failed: " + error);
      finish(r, STATUS_FAILED, ERROR_HTTP_DATA_ERROR);
    }
  }
  private void finish(Record r, int status, int reason) {
    synchronized (this) {
      if (r.removed) return;
      r.status = status;
      r.reason = reason;
      if (status == STATUS_SUCCESSFUL && r.total < 0) r.total = r.done;
      r.modified = System.currentTimeMillis();
      r.task = null;
      save();
    }
    // DownloadNotifier/DownloadInfo.sendIntentIfRequested: the app's package receives completion.
    Intent intent = new Intent(ACTION_DOWNLOAD_COMPLETE);
    if (packageName != null) intent.setPackage(packageName);
    intent.putExtra(EXTRA_DOWNLOAD_ID, r.id);
    broadcast.accept(intent);
  }

  private static final String[] FIELDS = {"uri", "destination", "title", "description", "mimeType", "headers"};
  private void load() {
    if (!store.isFile()) return;
    Properties p = new Properties();
    try (Reader in = Files.newBufferedReader(store.toPath(), StandardCharsets.UTF_8)) {
      p.load(in);
    } catch (IOException | IllegalArgumentException error) {
      android.util.Log.w("DownloadManager", "Download records unreadable: " + error);
      return;
    }
    synchronized (this) {
      nextId = Long.parseLong(p.getProperty("next", "1"));
      for (String key : p.stringPropertyNames()) {
        if (!key.endsWith(".status")) continue;
        String prefix = key.substring(0, key.length() - ".status".length());
        try {
          Record r = new Record();
          r.id = Long.parseLong(prefix);
          r.uri = p.getProperty(prefix + ".uri");
          r.destination = p.getProperty(prefix + ".destination");
          r.title = p.getProperty(prefix + ".title");
          r.description = p.getProperty(prefix + ".description");
          r.mimeType = p.getProperty(prefix + ".mimeType");
          r.headers = p.getProperty(prefix + ".headers", "");
          r.status = Integer.parseInt(p.getProperty(key));
          r.reason = Integer.parseInt(p.getProperty(prefix + ".reason", "0"));
          r.total = Long.parseLong(p.getProperty(prefix + ".total", "-1"));
          r.done = Long.parseLong(p.getProperty(prefix + ".done", "0"));
          r.modified = Long.parseLong(p.getProperty(prefix + ".modified", "0"));
          if (r.uri == null) continue;
          records.put(r.id, r);
          nextId = Math.max(nextId, r.id + 1);
        } catch (NumberFormatException ignored) {}
      }
    }
  }
  // Called with this locked.
  private void save() {
    Properties p = new Properties();
    p.setProperty("next", Long.toString(nextId));
    for (Record r : records.values()) {
      String prefix = Long.toString(r.id);
      String[] values = {r.uri, r.destination, r.title, r.description, r.mimeType, r.headers};
      for (int i = 0; i < FIELDS.length; i++) if (values[i] != null) p.setProperty(prefix + "." + FIELDS[i], values[i]);
      p.setProperty(prefix + ".status", Integer.toString(r.status));
      p.setProperty(prefix + ".reason", Integer.toString(r.reason));
      p.setProperty(prefix + ".total", Long.toString(r.total));
      p.setProperty(prefix + ".done", Long.toString(r.done));
      p.setProperty(prefix + ".modified", Long.toString(r.modified));
    }
    try {
      File parent = store.getAbsoluteFile().getParentFile();
      if (parent != null) parent.mkdirs();
      File temp = new File(store.getPath() + ".tmp");
      try (Writer out = Files.newBufferedWriter(temp.toPath(), StandardCharsets.UTF_8)) {
        p.store(out, null);
      }
      Files.move(temp.toPath(), store.toPath(), StandardCopyOption.REPLACE_EXISTING);
    } catch (IOException error) {
      android.util.Log.w("DownloadManager", "Download records not saved: " + error);
    }
  }
}
