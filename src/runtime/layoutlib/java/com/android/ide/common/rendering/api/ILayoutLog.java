package com.android.ide.common.rendering.api;

/**
 * The log layoutlib's natives and delegates write to: Android Studio's
 * layoutlib-api interface, reduced to the calls a Venture 2D process makes.
 */
public interface ILayoutLog {
  String TAG_UNSUPPORTED = "unsupported";
  String TAG_BROKEN = "broken";
  String TAG_RESOURCES_PREFIX = "resources.";
  String TAG_RESOURCES_FORMAT = "resources.format";

  void logAndroidFramework(int priority, String tag, String message);

  default void warning(String tag, String message, Object viewCookie, Object data) {
    logAndroidFramework(5, tag == null ? "layoutlib" : tag, message);
  }

  default void fidelityWarning(String tag, String message, Throwable throwable, Object viewCookie, Object data) {
    logAndroidFramework(5, tag == null ? "layoutlib" : tag, throwable == null ? message : message + ": " + throwable);
  }

  default void error(String tag, String message, Object viewCookie, Object data) {
    logAndroidFramework(6, tag == null ? "layoutlib" : tag, message);
  }

  default void error(String tag, String message, Throwable throwable, Object viewCookie, Object data) {
    logAndroidFramework(6, tag == null ? "layoutlib" : tag, throwable == null ? message : message + ": " + throwable);
  }
}
