package android.app;

import android.os.Parcel;
import android.os.Parcelable;
import java.io.InputStream;

/** AOSP android.app.ApplicationExitInfo: one recorded death of an earlier process. */
public final class ApplicationExitInfo implements Parcelable {
  public static final int REASON_UNKNOWN = 0, REASON_EXIT_SELF = 1, REASON_SIGNALED = 2,
      REASON_LOW_MEMORY = 3, REASON_CRASH = 4, REASON_CRASH_NATIVE = 5, REASON_ANR = 6,
      REASON_INITIALIZATION_FAILURE = 7, REASON_PERMISSION_CHANGE = 8, REASON_EXCESSIVE_RESOURCE_USAGE = 9,
      REASON_USER_REQUESTED = 10, REASON_USER_STOPPED = 11, REASON_DEPENDENCY_DIED = 12, REASON_OTHER = 13,
      REASON_FREEZER = 14, REASON_PACKAGE_STATE_CHANGE = 15, REASON_PACKAGE_UPDATED = 16;
  private int pid, realUid, packageUid, definingUid, reason, status, importance;
  private long pss, rss, timestamp;
  private String processName, description;

  ApplicationExitInfo() {}
  public int getPid() { return pid; }
  public int getRealUid() { return realUid; }
  public int getPackageUid() { return packageUid; }
  public int getDefiningUid() { return definingUid; }
  public String getProcessName() { return processName; }
  public int getReason() { return reason; }
  public int getStatus() { return status; }
  public int getImportance() { return importance; }
  public long getPss() { return pss; }
  public long getRss() { return rss; }
  public long getTimestamp() { return timestamp; }
  public String getDescription() { return description; }
  public byte[] getProcessStateSummary() { return null; }
  public InputStream getTraceInputStream() { return null; }
  public int describeContents() { return 0; }
  public void writeToParcel(Parcel dest, int flags) { dest.writeParcelable(this, flags); }
  public static final Creator<ApplicationExitInfo> CREATOR = new Creator<>() {
    public ApplicationExitInfo createFromParcel(Parcel in) { return in.readParcelable(null); }
    public ApplicationExitInfo[] newArray(int size) { return new ApplicationExitInfo[size]; }
  };
}
