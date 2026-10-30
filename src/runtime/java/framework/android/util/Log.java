package android.util;
import com.venture.runtime.RuntimeEnvironment;
public final class Log {
  public static final int VERBOSE = 2, DEBUG = 3, INFO = 4, WARN = 5, ERROR = 6, ASSERT = 7;
  public static int println(int p, String tag, String msg) {
    com.venture.runtime.ProcessCommands.log(p, tag, String.valueOf(msg));
    RuntimeEnvironment.host().log(p, tag, String.valueOf(msg));
    return msg == null ? 0 : msg.length();
  }
  public static boolean isLoggable(String tag, int level) {
    return level >= INFO;
  }
  public static int v(String t, String m) {
    return println(VERBOSE, t, m);
  }
  public static int d(String t, String m) {
    return println(DEBUG, t, m);
  }
  public static int i(String t, String m) {
    return println(INFO, t, m);
  }
  public static int w(String t, String m) {
    return println(WARN, t, m);
  }
  public static int e(String t, String m) {
    return println(ERROR, t, m);
  }
  public static int wtf(String t, String m) {
    return println(ASSERT, t, m);
  }
  public static int wtf(String t, Throwable e) {
    return wtf(t, e == null ? "" : e.getMessage(), e);
  }
  public static int wtf(String t, String m, Throwable e) {
    return wtf(t, m + "\n" + getStackTraceString(e));
  }
  public static String getStackTraceString(Throwable e) {
    // As Android: no throwable (or a network-unavailable one) prints nothing.
    for (Throwable t = e; t != null; t = t.getCause())
      if (t instanceof java.net.UnknownHostException) return "";
    if (e == null) return "";
    java.io.StringWriter w = new java.io.StringWriter();
    e.printStackTrace(new java.io.PrintWriter(w));
    return w.toString();
  }
  public static int e(String t, String m, Throwable e) {
    return e(t, m + "\n" + getStackTraceString(e));
  }
  public static int w(String t, String m, Throwable e) {
    return w(t, m + "\n" + getStackTraceString(e));
  }
  public static int d(String t, String m, Throwable e) {
    return d(t, m + "\n" + getStackTraceString(e));
  }
  public static int i(String t, String m, Throwable e) {
    return i(t, m + "\n" + getStackTraceString(e));
  }
  public static int v(String t, String m, Throwable e) {
    return v(t, m + "\n" + getStackTraceString(e));
  }
  public static int w(String t, Throwable e) {
    return w(t, getStackTraceString(e));
  }
}
