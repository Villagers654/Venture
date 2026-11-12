package android.app;

import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import com.sun.net.httpserver.HttpServer;
import java.io.File;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

/**
 * DownloadManager against a local HTTP server: enqueue, redirect, completion broadcast, the
 * query cursor an app reads (AMAZE VR Concerts' downloader), failures, removal and records that
 * a new service instance (the next app start) still answers for.
 */
public final class DownloadManagerTest {
  private static int checks;
  private static void check(boolean ok, String message) {
    checks++;
    if (!ok) throw new AssertionError(message);
  }
  private static long completed(LinkedBlockingQueue<Intent> intents) throws InterruptedException {
    Intent intent = intents.poll(20, TimeUnit.SECONDS);
    check(intent != null, "completion broadcast arrives");
    check(DownloadManager.ACTION_DOWNLOAD_COMPLETE.equals(intent.getAction()), "completion action");
    check("test.game".equals(intent.getPackage()), "completion goes to the app's package");
    return intent.getLongExtra(DownloadManager.EXTRA_DOWNLOAD_ID, -1);
  }
  private static Cursor row(DownloadManager manager, long id) {
    Cursor cursor = manager.query(new DownloadManager.Query().setFilterById(id));
    check(cursor.moveToFirst(), "record " + id + " is queryable");
    return cursor;
  }
  public static void main(String[] args) throws Exception {
    byte[] body = new byte[300_000];
    for (int i = 0; i < body.length; i++) body[i] = (byte) (i * 31);
    List<String> headers = new CopyOnWriteArrayList<>();
    HttpServer server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
    server.createContext("/file.bin", exchange -> {
      headers.add(String.valueOf(exchange.getRequestHeaders().getFirst("X-Test")));
      exchange.sendResponseHeaders(200, body.length);
      try (OutputStream out = exchange.getResponseBody()) { out.write(body); }
    });
    server.createContext("/moved", exchange -> {
      exchange.getResponseHeaders().add("Location", "/file.bin");
      exchange.sendResponseHeaders(302, -1);
      exchange.close();
    });
    server.createContext("/missing", exchange -> {
      exchange.sendResponseHeaders(404, -1);
      exchange.close();
    });
    server.start();
    String base = "http://127.0.0.1:" + server.getAddress().getPort();
    Path root = Files.createTempDirectory("venture-downloads");
    File store = root.resolve("records").toFile();
    var intents = new LinkedBlockingQueue<Intent>();
    try {
      try {
        new DownloadManager.Request(Uri.parse("ftp://example.com/a"));
        check(false, "non-HTTP URIs are rejected");
      } catch (IllegalArgumentException expected) {}

      var manager = new DownloadManager(store, root.resolve("default").toFile(), intents::add, "test.game");
      File target = root.resolve("content/S01.mp4").toFile();
      long id = manager.enqueue(new DownloadManager.Request(Uri.parse(base + "/moved"))
          .setTitle("S01").setDescription("S01.mp4").addRequestHeader("X-Test", "yes")
          .setNotificationVisibility(DownloadManager.Request.VISIBILITY_HIDDEN)
          .setDestinationUri(Uri.fromFile(target)).setAllowedOverMetered(true));
      check(completed(intents) == id, "completion names the download");
      check(headers.contains("yes"), "request headers reach the server after a redirect");
      check(java.util.Arrays.equals(Files.readAllBytes(target.toPath()), body), "the destination holds the body");
      Cursor cursor = row(manager, id);
      check(cursor.getInt(cursor.getColumnIndex(DownloadManager.COLUMN_STATUS)) == DownloadManager.STATUS_SUCCESSFUL,
          "successful status");
      check(cursor.getLong(cursor.getColumnIndex(DownloadManager.COLUMN_TOTAL_SIZE_BYTES)) == body.length
          && cursor.getLong(cursor.getColumnIndex(DownloadManager.COLUMN_BYTES_DOWNLOADED_SO_FAR)) == body.length,
          "sizes");
      check("S01".equals(cursor.getString(cursor.getColumnIndex(DownloadManager.COLUMN_TITLE))), "title");
      String local = cursor.getString(cursor.getColumnIndex(DownloadManager.COLUMN_LOCAL_URI));
      check(Uri.fromFile(target).toString().equals(local), "local URI is the destination");
      check(manager.getUriForDownloadedFile(id) != null, "downloaded file URI");
      cursor.close();

      long missing = manager.enqueue(new DownloadManager.Request(Uri.parse(base + "/missing")));
      check(completed(intents) == missing, "a failed download also completes");
      cursor = row(manager, missing);
      check(cursor.getInt(cursor.getColumnIndex(DownloadManager.COLUMN_STATUS)) == DownloadManager.STATUS_FAILED
          && cursor.getInt(cursor.getColumnIndex(DownloadManager.COLUMN_REASON)) == 404, "HTTP failure reason");
      cursor.close();

      Cursor running = manager.query(new DownloadManager.Query().setFilterByStatus(
          DownloadManager.STATUS_RUNNING | DownloadManager.STATUS_PENDING | DownloadManager.STATUS_PAUSED));
      check(running.getCount() == 0, "no downloads in progress");
      running.close();
      check(manager.query(new DownloadManager.Query()).getCount() == 2, "an unfiltered query lists every download");

      // The next app start: records persist.
      var restarted = new DownloadManager(store, root.resolve("default").toFile(), intents::add, "test.game");
      cursor = row(restarted, id);
      check(cursor.getInt(cursor.getColumnIndex(DownloadManager.COLUMN_STATUS)) == DownloadManager.STATUS_SUCCESSFUL,
          "a finished download survives a restart");
      cursor.close();
      long next = restarted.enqueue(new DownloadManager.Request(Uri.parse(base + "/file.bin")));
      check(next > missing, "IDs keep increasing across restarts");
      check(completed(intents) == next, "the restarted service downloads");
      File defaultFile = restarted.ventureFile(next);
      check(defaultFile != null && defaultFile.length() == body.length, "a download without destination is stored");

      check(restarted.remove(id, 12345) == 1, "remove counts existing downloads");
      check(!target.exists(), "removing a download deletes its file");
      check(restarted.query(new DownloadManager.Query().setFilterById(id)).getCount() == 0, "removed record");
      check(intents.isEmpty(), "no further broadcasts");
    } finally {
      server.stop(0);
    }
    System.out.println("DownloadManagerTest: " + checks + " checks passed");
  }
}
