package android.webkit;

import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/**
 * AOSP android.webkit.MimeTypeMap: two-way extension/MIME lookup over the
 * platform table (libcore MimeMap). The table holds the common media,
 * document, archive and web types; the first extension listed for a type is
 * its preferred extension, as in Android's mime.types data.
 */
public class MimeTypeMap {
  private static final MimeTypeMap singleton = new MimeTypeMap();
  private static final Map<String, String> byExtension = new HashMap<>();
  private static final Map<String, String> byType = new HashMap<>();
  private static void add(String type, String... extensions) {
    byType.putIfAbsent(type, extensions[0]);
    for (String e : extensions) byExtension.putIfAbsent(e, type);
  }
  static {
    add("text/plain", "txt", "text", "log", "conf");
    add("text/html", "html", "htm");
    add("text/css", "css");
    add("text/csv", "csv");
    add("text/xml", "xml");
    add("text/javascript", "js");
    add("application/javascript", "js");
    add("application/json", "json");
    add("application/pdf", "pdf");
    add("application/zip", "zip");
    add("application/gzip", "gz", "tgz");
    add("application/x-7z-compressed", "7z");
    add("application/vnd.rar", "rar");
    add("application/x-tar", "tar");
    add("application/octet-stream", "bin");
    add("application/vnd.android.package-archive", "apk");
    add("application/x-x509-ca-cert", "crt", "der");
    add("application/xhtml+xml", "xhtml");
    add("application/rss+xml", "rss");
    add("application/wasm", "wasm");
    add("application/x-mpegURL", "m3u8");
    add("application/vnd.apple.mpegurl", "m3u8");
    add("application/dash+xml", "mpd");
    add("application/x-subrip", "srt");
    add("text/vtt", "vtt");
    add("image/jpeg", "jpg", "jpeg", "jpe");
    add("image/png", "png");
    add("image/gif", "gif");
    add("image/bmp", "bmp");
    add("image/webp", "webp");
    add("image/svg+xml", "svg", "svgz");
    add("image/x-icon", "ico");
    add("image/heic", "heic");
    add("image/heif", "heif");
    add("image/avif", "avif");
    add("image/tiff", "tiff", "tif");
    add("audio/mpeg", "mp3", "mpga", "mpega", "mp2");
    add("audio/mp4", "m4a");
    add("audio/aac", "aac");
    add("audio/ogg", "oga", "ogg", "opus", "spx");
    add("audio/flac", "flac");
    add("audio/x-wav", "wav");
    add("audio/midi", "mid", "midi");
    add("audio/amr", "amr");
    add("audio/webm", "weba");
    add("video/mp4", "mp4", "m4v");
    add("video/webm", "webm");
    add("video/x-matroska", "mkv");
    add("video/quicktime", "mov", "qt");
    add("video/3gpp", "3gp");
    add("video/x-msvideo", "avi");
    add("video/mpeg", "mpeg", "mpg", "mpe");
    add("video/mp2ts", "ts");
    add("video/x-flv", "flv");
    add("video/x-ms-wmv", "wmv");
    add("font/ttf", "ttf");
    add("font/otf", "otf");
    add("font/woff", "woff");
    add("font/woff2", "woff2");
  }
  private MimeTypeMap() {}
  public static MimeTypeMap getSingleton() { return singleton; }
  /** AOSP: the part after the last dot of the URL's file name, without query or fragment. */
  public static String getFileExtensionFromUrl(String url) {
    if (url == null || url.isEmpty()) return "";
    int fragment = url.lastIndexOf('#');
    if (fragment > 0) url = url.substring(0, fragment);
    int query = url.lastIndexOf('?');
    if (query > 0) url = url.substring(0, query);
    int slash = url.lastIndexOf('/');
    String name = slash >= 0 ? url.substring(slash + 1) : url;
    if (!name.isEmpty() && name.matches("[a-zA-Z_0-9.\\-()%]+")) {
      int dot = name.lastIndexOf('.');
      if (dot >= 0) return name.substring(dot + 1);
    }
    return "";
  }
  public boolean hasMimeType(String mimeType) {
    return mimeType != null && !mimeType.isEmpty() && byType.containsKey(mimeType.toLowerCase(Locale.ROOT));
  }
  public String getMimeTypeFromExtension(String extension) {
    return extension == null || extension.isEmpty() ? null : byExtension.get(extension.toLowerCase(Locale.ROOT));
  }
  public boolean hasExtension(String extension) { return getMimeTypeFromExtension(extension) != null; }
  public String getExtensionFromMimeType(String mimeType) {
    return mimeType == null || mimeType.isEmpty() ? null : byType.get(mimeType.toLowerCase(Locale.ROOT));
  }
}
