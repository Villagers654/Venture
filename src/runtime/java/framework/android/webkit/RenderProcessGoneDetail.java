package android.webkit;

/** AOSP android.webkit.RenderProcessGoneDetail (API 26). */
public abstract class RenderProcessGoneDetail {
  public RenderProcessGoneDetail() {}
  public abstract boolean didCrash();
  public abstract int rendererPriorityAtExit();
}
