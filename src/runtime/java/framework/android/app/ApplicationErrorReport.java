package android.app;

import android.content.ComponentName;
import android.content.Context;
import android.os.Parcel;
import android.os.Parcelable;
import java.io.PrintWriter;
import java.io.StringWriter;

/**
 * AOSP android.app.ApplicationErrorReport: what a crash, ANR or battery report
 * carries. Unity's crash handler looks the class up at startup and asks
 * getErrorReportReceiver for the store's error receiver; Venture has none.
 */
public class ApplicationErrorReport implements Parcelable {
  public static final int TYPE_NONE = 0, TYPE_CRASH = 1, TYPE_ANR = 2, TYPE_BATTERY = 3,
      TYPE_RUNNING_SERVICE = 5;

  public int type;
  public String packageName, installerPackageName, processName;
  public long time;
  public boolean systemApp;
  public CrashInfo crashInfo;
  public AnrInfo anrInfo;
  public BatteryInfo batteryInfo;
  public RunningServiceInfo runningServiceInfo;

  public ApplicationErrorReport() {}

  /** Hidden in AOSP: the installer's APP_ERROR receiver, null when there is none. */
  public static ComponentName getErrorReportReceiver(Context context, String packageName, int appFlags) {
    return null;
  }

  public int describeContents() { return 0; }
  public void writeToParcel(Parcel dest, int flags) { dest.writeParcelable(this, flags); }
  public static final Creator<ApplicationErrorReport> CREATOR = new Creator<>() {
    public ApplicationErrorReport createFromParcel(Parcel in) { return in.readParcelable(null); }
    public ApplicationErrorReport[] newArray(int size) { return new ApplicationErrorReport[size]; }
  };

  public static class CrashInfo {
    public String exceptionClassName, exceptionMessage, throwFileName, throwClassName, throwMethodName,
        stackTrace;
    public int throwLineNumber;

    public CrashInfo() {}
    public CrashInfo(Throwable tr) {
      StringWriter trace = new StringWriter();
      tr.printStackTrace(new PrintWriter(trace));
      stackTrace = trace.toString();
      exceptionMessage = tr.getMessage();
      Throwable root = tr;
      while (root.getCause() != null) {
        root = root.getCause();
        if (root.getStackTrace() != null && root.getStackTrace().length > 0) tr = root;
        if (root.getMessage() != null && !root.getMessage().isEmpty()) exceptionMessage = root.getMessage();
      }
      exceptionClassName = root.getClass().getName();
      StackTraceElement[] frames = tr.getStackTrace();
      if (frames != null && frames.length > 0) {
        throwFileName = frames[0].getFileName();
        throwClassName = frames[0].getClassName();
        throwMethodName = frames[0].getMethodName();
        throwLineNumber = frames[0].getLineNumber();
      } else {
        throwFileName = throwClassName = throwMethodName = "unknown";
      }
    }
    public CrashInfo(Parcel in) {}
    public void writeToParcel(Parcel dest, int flags) {}
  }

  public static class AnrInfo {
    public String activity, cause, info;

    public AnrInfo() {}
    public AnrInfo(Parcel in) {}
    public void writeToParcel(Parcel dest, int flags) {}
  }

  public static class BatteryInfo {
    public int usagePercent;
    public long durationMicros;
    public String usageDetails, checkinDetails;

    public BatteryInfo() {}
    public BatteryInfo(Parcel in) {}
    public void writeToParcel(Parcel dest, int flags) {}
  }

  public static class RunningServiceInfo {
    public long durationMillis;
    public String serviceDetails;

    public RunningServiceInfo() {}
    public RunningServiceInfo(Parcel in) {}
    public void writeToParcel(Parcel dest, int flags) {}
  }
}
