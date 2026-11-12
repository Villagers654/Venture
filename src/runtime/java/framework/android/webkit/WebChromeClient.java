package android.webkit;

import android.graphics.Bitmap;
import android.net.Uri;
import android.os.Message;
import android.view.View;

/** AOSP android.webkit.WebChromeClient: browser UI callbacks, with AOSP's default behaviour. */
public class WebChromeClient {
  public interface CustomViewCallback {
    void onCustomViewHidden();
  }
  public abstract static class FileChooserParams {
    public static final int MODE_OPEN = 0, MODE_OPEN_MULTIPLE = 1, MODE_OPEN_FOLDER = 2, MODE_SAVE = 3;
    public FileChooserParams() {}
    /** AOSP (WebViewChromium): null unless RESULT_OK with a data URI. */
    public static Uri[] parseResult(int resultCode, android.content.Intent data) {
      if (resultCode != android.app.Activity.RESULT_OK || data == null) return null;
      Uri uri = data.getData();
      return uri == null ? null : new Uri[] {uri};
    }
    public abstract int getMode();
    public abstract String[] getAcceptTypes();
    public abstract boolean isCaptureEnabled();
    public abstract CharSequence getTitle();
    public abstract String getFilenameHint();
    public abstract android.content.Intent createIntent();
  }
  public WebChromeClient() {}
  public void onProgressChanged(WebView view, int newProgress) {}
  public void onReceivedTitle(WebView view, String title) {}
  public void onReceivedIcon(WebView view, Bitmap icon) {}
  public void onReceivedTouchIconUrl(WebView view, String url, boolean precomposed) {}
  public void onShowCustomView(View view, CustomViewCallback callback) {}
  @Deprecated public void onShowCustomView(View view, int requestedOrientation, CustomViewCallback callback) {}
  public void onHideCustomView() {}
  public boolean onCreateWindow(WebView view, boolean isDialog, boolean isUserGesture, Message resultMsg) {
    return false;
  }
  public void onRequestFocus(WebView view) {}
  public void onCloseWindow(WebView window) {}
  public boolean onJsAlert(WebView view, String url, String message, JsResult result) { return false; }
  public boolean onJsConfirm(WebView view, String url, String message, JsResult result) { return false; }
  public boolean onJsPrompt(WebView view, String url, String message, String defaultValue, JsPromptResult result) {
    return false;
  }
  public boolean onJsBeforeUnload(WebView view, String url, String message, JsResult result) { return false; }
  public void onGeolocationPermissionsShowPrompt(String origin, GeolocationPermissions.Callback callback) {}
  public void onGeolocationPermissionsHidePrompt() {}
  public void onPermissionRequest(PermissionRequest request) { request.deny(); }
  public void onPermissionRequestCanceled(PermissionRequest request) {}
  @Deprecated public boolean onJsTimeout() { return true; }
  @Deprecated public void onConsoleMessage(String message, int lineNumber, String sourceID) {}
  public boolean onConsoleMessage(ConsoleMessage consoleMessage) {
    onConsoleMessage(consoleMessage.message(), consoleMessage.lineNumber(), consoleMessage.sourceId());
    return false;
  }
  public Bitmap getDefaultVideoPoster() { return null; }
  public View getVideoLoadingProgressView() { return null; }
  public void getVisitedHistory(ValueCallback<String[]> callback) {}
  public boolean onShowFileChooser(WebView webView, ValueCallback<Uri[]> filePathCallback,
      FileChooserParams fileChooserParams) {
    return false;
  }
}
