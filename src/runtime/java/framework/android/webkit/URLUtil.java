package android.webkit;

import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** AOSP android.webkit.URLUtil: URL classification and download file naming. */
public final class URLUtil {
  public URLUtil() {}
  private static boolean starts(String url, String prefix) {
    return url != null && url.regionMatches(true, 0, prefix, 0, prefix.length());
  }
  public static boolean isAssetUrl(String url) { return url != null && url.startsWith("file:///android_asset/"); }
  public static boolean isFileUrl(String url) { return starts(url, "file://") && !isAssetUrl(url); }
  public static boolean isAboutUrl(String url) { return starts(url, "about:"); }
  public static boolean isDataUrl(String url) { return starts(url, "data:"); }
  public static boolean isJavaScriptUrl(String url) { return starts(url, "javascript:"); }
  public static boolean isHttpUrl(String url) { return url != null && url.length() > 6 && starts(url, "http://"); }
  public static boolean isHttpsUrl(String url) { return url != null && url.length() > 7 && starts(url, "https://"); }
  public static boolean isNetworkUrl(String url) {
    return url != null && !url.isEmpty() && (isHttpUrl(url) || isHttpsUrl(url));
  }
  public static boolean isContentUrl(String url) { return starts(url, "content:"); }
  public static boolean isValidUrl(String url) {
    return url != null && !url.isEmpty() && (isAssetUrl(url) || isFileUrl(url) || isAboutUrl(url)
        || isHttpUrl(url) || isHttpsUrl(url) || isJavaScriptUrl(url) || isContentUrl(url));
  }
  public static String stripAnchor(String url) {
    int anchor = url.indexOf('#');
    return anchor != -1 ? url.substring(0, anchor) : url;
  }
  private static final Pattern DISPOSITION =
      Pattern.compile("attachment;\\s*filename\\s*=\\s*(\"?)([^\"]*)\\1\\s*$", Pattern.CASE_INSENSITIVE);
  static String parseContentDisposition(String contentDisposition) {
    if (contentDisposition == null) return null;
    Matcher m = DISPOSITION.matcher(contentDisposition);
    return m.find() ? m.group(2) : null;
  }
  /** AOSP guessFileName: disposition name, else the URL's last path segment, else "downloadfile". */
  public static String guessFileName(String url, String contentDisposition, String mimeType) {
    String filename = null, extension = null;
    if (contentDisposition != null) {
      filename = parseContentDisposition(contentDisposition);
      if (filename != null) {
        int index = filename.lastIndexOf('/') + 1;
        if (index > 0) filename = filename.substring(index);
      }
    }
    if (filename == null && url != null) {
      String decoded = url;
      int query = decoded.indexOf('?');
      if (query > 0) decoded = decoded.substring(0, query);
      try { decoded = java.net.URLDecoder.decode(decoded, "UTF-8"); } catch (Exception ignored) {}
      if (!decoded.endsWith("/")) {
        int index = decoded.lastIndexOf('/') + 1;
        if (index > 0) filename = decoded.substring(index);
      }
    }
    if (filename == null || filename.isEmpty()) filename = "downloadfile";
    int dot = filename.indexOf('.');
    if (dot < 0) {
      if (mimeType != null) {
        extension = MimeTypeMap.getSingleton().getExtensionFromMimeType(mimeType);
        if (extension != null) extension = "." + extension;
      }
      if (extension == null) {
        if (mimeType != null && mimeType.toLowerCase(Locale.ROOT).startsWith("text/"))
          extension = mimeType.equalsIgnoreCase("text/html") ? ".html" : ".txt";
        else
          extension = ".bin";
      }
    } else {
      if (mimeType != null) {
        int lastDot = filename.lastIndexOf('.');
        String typeFromExt = MimeTypeMap.getSingleton().getMimeTypeFromExtension(filename.substring(lastDot + 1));
        if (typeFromExt != null && !typeFromExt.equalsIgnoreCase(mimeType)) {
          extension = MimeTypeMap.getSingleton().getExtensionFromMimeType(mimeType);
          if (extension != null) extension = "." + extension;
        }
      }
      if (extension == null) extension = filename.substring(dot);
      filename = filename.substring(0, dot);
    }
    return filename + extension;
  }
}
