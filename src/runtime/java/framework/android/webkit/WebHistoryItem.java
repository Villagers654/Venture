package android.webkit;

/** AOSP android.webkit.WebHistoryItem: one entry of a WebView's back/forward list. */
public abstract class WebHistoryItem implements Cloneable {
  public WebHistoryItem() {}
  public abstract String getUrl();
  public abstract String getOriginalUrl();
  public abstract String getTitle();
  public abstract android.graphics.Bitmap getFavicon();
  @Override protected abstract WebHistoryItem clone();
}
