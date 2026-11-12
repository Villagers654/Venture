package android.webkit;

/** AOSP android.webkit.WebBackForwardList: a snapshot of a WebView's navigation history. */
public abstract class WebBackForwardList implements Cloneable, java.io.Serializable {
  public WebBackForwardList() {}
  public abstract WebHistoryItem getCurrentItem();
  public abstract int getCurrentIndex();
  public abstract WebHistoryItem getItemAtIndex(int index);
  public abstract int getSize();
  @Override protected abstract WebBackForwardList clone();
}
