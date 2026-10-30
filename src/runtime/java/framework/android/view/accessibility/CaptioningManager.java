package android.view.accessibility;

import java.util.Locale;

/** AOSP android.view.accessibility.CaptioningManager with the system caption setting off (Quest default). */
public class CaptioningManager {
  public CaptioningManager(android.content.Context context) {}
  public boolean isEnabled() { return false; }
  public boolean isSystemAudioCaptioningEnabled() { return false; }
  public Locale getLocale() { return null; }
  public float getFontScale() { return 1f; }
  public CaptionStyle getUserStyle() { return CaptionStyle.DEFAULT; }
  public void addCaptioningChangeListener(CaptioningChangeListener listener) {}
  public void removeCaptioningChangeListener(CaptioningChangeListener listener) {}
  public abstract static class CaptioningChangeListener {
    public void onEnabledChanged(boolean enabled) {}
    public void onUserStyleChanged(CaptionStyle userStyle) {}
    public void onLocaleChanged(Locale locale) {}
    public void onFontScaleChanged(float fontScale) {}
  }
  public static final class CaptionStyle {
    public static final int EDGE_TYPE_UNSPECIFIED = -1, EDGE_TYPE_NONE = 0, EDGE_TYPE_OUTLINE = 1,
        EDGE_TYPE_DROP_SHADOW = 2, EDGE_TYPE_RAISED = 3, EDGE_TYPE_DEPRESSED = 4;
    static final CaptionStyle DEFAULT = new CaptionStyle(0xFFFFFFFF, 0xFF000000, EDGE_TYPE_NONE, 0xFF000000, 0);
    public final int foregroundColor, backgroundColor, edgeType, edgeColor, windowColor;
    private CaptionStyle(int foreground, int background, int edge, int edgeColor, int window) {
      foregroundColor = foreground; backgroundColor = background; edgeType = edge; this.edgeColor = edgeColor;
      windowColor = window;
    }
    public boolean hasBackgroundColor() { return true; }
    public boolean hasForegroundColor() { return true; }
    public boolean hasEdgeType() { return true; }
    public boolean hasEdgeColor() { return true; }
    public boolean hasWindowColor() { return true; }
  }
}
