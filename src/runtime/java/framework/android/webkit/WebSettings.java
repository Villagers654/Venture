package android.webkit;

/**
 * AOSP android.webkit.WebSettings: the per-WebView settings object. Values are
 * recorded and read back with AOSP defaults; WebView.getSettings returns one.
 */
public abstract class WebSettings {
  public enum LayoutAlgorithm { NORMAL, SINGLE_COLUMN, NARROW_COLUMNS, TEXT_AUTOSIZING }
  public enum PluginState { ON, ON_DEMAND, OFF }
  public enum RenderPriority { NORMAL, HIGH, LOW }
  public enum ZoomDensity {
    FAR(150), MEDIUM(100), CLOSE(75);
    final int value;
    ZoomDensity(int size) { value = size; }
    public int getValue() { return value; }
  }
  public enum TextSize {
    SMALLEST(50), SMALLER(75), NORMAL(100), LARGER(150), LARGEST(200);
    final int value;
    TextSize(int size) { value = size; }
  }
  public static final int LOAD_DEFAULT = -1, LOAD_NORMAL = 0, LOAD_CACHE_ELSE_NETWORK = 1,
      LOAD_NO_CACHE = 2, LOAD_CACHE_ONLY = 3;
  public static final int MIXED_CONTENT_ALWAYS_ALLOW = 0, MIXED_CONTENT_NEVER_ALLOW = 1,
      MIXED_CONTENT_COMPATIBILITY_MODE = 2;
  public static final int FORCE_DARK_OFF = 0, FORCE_DARK_AUTO = 1, FORCE_DARK_ON = 2;
  public static final int MENU_ITEM_NONE = 0, MENU_ITEM_SHARE = 1, MENU_ITEM_WEB_SEARCH = 2,
      MENU_ITEM_PROCESS_TEXT = 4;

  /** Android WebView's user agent: the device's Chrome UA with the "; wv" WebView marker. */
  public static String getDefaultUserAgent(android.content.Context context) {
    return "Mozilla/5.0 (Linux; Android " + android.os.Build.VERSION.RELEASE + "; "
        + android.os.Build.MODEL + " Build/" + android.os.Build.ID
        + "; wv) AppleWebKit/537.36 (KHTML, like Gecko) Version/4.0 Chrome/120.0.6099.230 Mobile Safari/537.36";
  }

  private boolean javaScript, domStorage, databaseEnabled, allowFileAccess, allowContentAccess = true,
      universalFileAccess, fileAccessFromFileUrls, builtInZoom, displayZoom = true, supportZoom = true,
      jsOpenWindows, multipleWindows, mediaGesture = true, saveFormData = true, savePassword,
      loadImages = true, blockNetworkImage, blockNetworkLoads, useWideViewPort, loadWithOverview,
      geolocation = true, offscreenPreRaster, safeBrowsing = true, appCache, lightTouch;
  private int cacheMode = LOAD_DEFAULT, mixedContent = MIXED_CONTENT_NEVER_ALLOW, forceDark = FORCE_DARK_AUTO,
      textZoom = 100, minimumFontSize = 8, minimumLogicalFontSize = 8, defaultFontSize = 16,
      defaultFixedFontSize = 13, disabledActionModeMenuItems = MENU_ITEM_NONE;
  private String userAgent, defaultTextEncoding = "UTF-8", standardFont = "sans-serif",
      fixedFont = "monospace", sansSerifFont = "sans-serif", serifFont = "serif",
      cursiveFont = "cursive", fantasyFont = "fantasy";
  private LayoutAlgorithm layoutAlgorithm = LayoutAlgorithm.NARROW_COLUMNS;
  private PluginState pluginState = PluginState.OFF;

  public WebSettings() {}

  public synchronized void setJavaScriptEnabled(boolean flag) { javaScript = flag; }
  public synchronized boolean getJavaScriptEnabled() { return javaScript; }
  public synchronized void setDomStorageEnabled(boolean flag) { domStorage = flag; }
  public synchronized boolean getDomStorageEnabled() { return domStorage; }
  public synchronized void setDatabaseEnabled(boolean flag) { databaseEnabled = flag; }
  public synchronized boolean getDatabaseEnabled() { return databaseEnabled; }
  @Deprecated public synchronized void setDatabasePath(String path) {}
  @Deprecated public synchronized String getDatabasePath() { return ""; }
  @Deprecated public synchronized void setAppCacheEnabled(boolean flag) { appCache = flag; }
  @Deprecated public synchronized void setAppCachePath(String path) {}
  @Deprecated public synchronized void setAppCacheMaxSize(long size) {}
  public void setAllowFileAccess(boolean allow) { allowFileAccess = allow; }
  public boolean getAllowFileAccess() { return allowFileAccess; }
  public void setAllowContentAccess(boolean allow) { allowContentAccess = allow; }
  public boolean getAllowContentAccess() { return allowContentAccess; }
  public void setAllowUniversalAccessFromFileURLs(boolean flag) { universalFileAccess = flag; }
  public boolean getAllowUniversalAccessFromFileURLs() { return universalFileAccess; }
  public void setAllowFileAccessFromFileURLs(boolean flag) { fileAccessFromFileUrls = flag; }
  public boolean getAllowFileAccessFromFileURLs() { return fileAccessFromFileUrls; }
  public void setSupportZoom(boolean support) { supportZoom = support; }
  public boolean supportZoom() { return supportZoom; }
  public void setBuiltInZoomControls(boolean enabled) { builtInZoom = enabled; }
  public boolean getBuiltInZoomControls() { return builtInZoom; }
  public void setDisplayZoomControls(boolean enabled) { displayZoom = enabled; }
  public boolean getDisplayZoomControls() { return displayZoom; }
  public synchronized void setJavaScriptCanOpenWindowsAutomatically(boolean flag) { jsOpenWindows = flag; }
  public synchronized boolean getJavaScriptCanOpenWindowsAutomatically() { return jsOpenWindows; }
  public synchronized void setSupportMultipleWindows(boolean support) { multipleWindows = support; }
  public synchronized boolean supportMultipleWindows() { return multipleWindows; }
  public void setMediaPlaybackRequiresUserGesture(boolean require) { mediaGesture = require; }
  public boolean getMediaPlaybackRequiresUserGesture() { return mediaGesture; }
  @Deprecated public void setSaveFormData(boolean save) { saveFormData = save; }
  @Deprecated public boolean getSaveFormData() { return saveFormData; }
  @Deprecated public void setSavePassword(boolean save) { savePassword = save; }
  @Deprecated public boolean getSavePassword() { return savePassword; }
  public void setCacheMode(int mode) { cacheMode = mode; }
  public int getCacheMode() { return cacheMode; }
  public void setMixedContentMode(int mode) { mixedContent = mode; }
  public int getMixedContentMode() { return mixedContent; }
  public void setForceDark(int mode) { forceDark = mode; }
  public int getForceDark() { return forceDark; }
  public synchronized void setTextZoom(int zoom) { textZoom = zoom; }
  public synchronized int getTextZoom() { return textZoom; }
  /** AOSP: null or empty restores the default user agent. */
  public synchronized void setUserAgentString(String ua) { userAgent = ua == null || ua.isEmpty() ? null : ua; }
  public synchronized String getUserAgentString() {
    return userAgent != null ? userAgent : getDefaultUserAgent(null);
  }
  public synchronized void setDefaultTextEncodingName(String encoding) { defaultTextEncoding = encoding; }
  public synchronized String getDefaultTextEncodingName() { return defaultTextEncoding; }
  public synchronized void setLoadsImagesAutomatically(boolean flag) { loadImages = flag; }
  public synchronized boolean getLoadsImagesAutomatically() { return loadImages; }
  public synchronized void setBlockNetworkImage(boolean flag) { blockNetworkImage = flag; }
  public synchronized boolean getBlockNetworkImage() { return blockNetworkImage; }
  public synchronized void setBlockNetworkLoads(boolean flag) { blockNetworkLoads = flag; }
  public synchronized boolean getBlockNetworkLoads() { return blockNetworkLoads; }
  public synchronized void setUseWideViewPort(boolean use) { useWideViewPort = use; }
  public synchronized boolean getUseWideViewPort() { return useWideViewPort; }
  public void setLoadWithOverviewMode(boolean overview) { loadWithOverview = overview; }
  public boolean getLoadWithOverviewMode() { return loadWithOverview; }
  public synchronized void setLayoutAlgorithm(LayoutAlgorithm l) { layoutAlgorithm = l; }
  public synchronized LayoutAlgorithm getLayoutAlgorithm() { return layoutAlgorithm; }
  @Deprecated public synchronized void setPluginState(PluginState state) { pluginState = state; }
  @Deprecated public synchronized PluginState getPluginState() { return pluginState; }
  public void setGeolocationEnabled(boolean flag) { geolocation = flag; }
  @Deprecated public void setGeolocationDatabasePath(String databasePath) {}
  public void setOffscreenPreRaster(boolean enabled) { offscreenPreRaster = enabled; }
  public boolean getOffscreenPreRaster() { return offscreenPreRaster; }
  public void setSafeBrowsingEnabled(boolean enabled) { safeBrowsing = enabled; }
  public boolean getSafeBrowsingEnabled() { return safeBrowsing; }
  @Deprecated public void setLightTouchEnabled(boolean enabled) { lightTouch = enabled; }
  @Deprecated public boolean getLightTouchEnabled() { return lightTouch; }
  @Deprecated public synchronized void setRenderPriority(RenderPriority priority) {}
  @Deprecated public void setEnableSmoothTransition(boolean enable) {}
  @Deprecated public void setNeedInitialFocus(boolean flag) {}
  @Deprecated public synchronized void setTextSize(TextSize t) { setTextZoom(t.value); }
  public synchronized void setMinimumFontSize(int size) { minimumFontSize = size; }
  public synchronized int getMinimumFontSize() { return minimumFontSize; }
  public synchronized void setMinimumLogicalFontSize(int size) { minimumLogicalFontSize = size; }
  public synchronized int getMinimumLogicalFontSize() { return minimumLogicalFontSize; }
  public synchronized void setDefaultFontSize(int size) { defaultFontSize = size; }
  public synchronized int getDefaultFontSize() { return defaultFontSize; }
  public synchronized void setDefaultFixedFontSize(int size) { defaultFixedFontSize = size; }
  public synchronized int getDefaultFixedFontSize() { return defaultFixedFontSize; }
  public synchronized void setStandardFontFamily(String font) { standardFont = font; }
  public synchronized String getStandardFontFamily() { return standardFont; }
  public synchronized void setFixedFontFamily(String font) { fixedFont = font; }
  public synchronized String getFixedFontFamily() { return fixedFont; }
  public synchronized void setSansSerifFontFamily(String font) { sansSerifFont = font; }
  public synchronized String getSansSerifFontFamily() { return sansSerifFont; }
  public synchronized void setSerifFontFamily(String font) { serifFont = font; }
  public synchronized String getSerifFontFamily() { return serifFont; }
  public synchronized void setCursiveFontFamily(String font) { cursiveFont = font; }
  public synchronized String getCursiveFontFamily() { return cursiveFont; }
  public synchronized void setFantasyFontFamily(String font) { fantasyFont = font; }
  public synchronized String getFantasyFontFamily() { return fantasyFont; }
  public void setDisabledActionModeMenuItems(int menuItems) { disabledActionModeMenuItems = menuItems; }
  public int getDisabledActionModeMenuItems() { return disabledActionModeMenuItems; }
}
