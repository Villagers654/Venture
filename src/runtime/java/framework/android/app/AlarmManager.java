package android.app;

import android.os.Handler;
import android.os.Looper;
import android.os.Parcel;
import android.os.Parcelable;
import android.os.SystemClock;
import java.util.ArrayList;
import java.util.List;

/**
 * AOSP android.app.AlarmManager for the one hosted process. Alarms fire in the process on its
 * main thread (listener alarms on their Handler) at the requested time. The device never
 * sleeps, so wakeup and while-idle variants behave alike. Like the app's jobs, alarms do not
 * outlive the process.
 */
public class AlarmManager {
  public static final int RTC_WAKEUP = 0, RTC = 1, ELAPSED_REALTIME_WAKEUP = 2, ELAPSED_REALTIME = 3;
  public static final long INTERVAL_FIFTEEN_MINUTES = 15 * 60 * 1000L,
      INTERVAL_HALF_HOUR = 2 * INTERVAL_FIFTEEN_MINUTES, INTERVAL_HOUR = 2 * INTERVAL_HALF_HOUR,
      INTERVAL_HALF_DAY = 12 * INTERVAL_HOUR, INTERVAL_DAY = 2 * INTERVAL_HALF_DAY;
  public static final String ACTION_NEXT_ALARM_CLOCK_CHANGED = "android.app.action.NEXT_ALARM_CLOCK_CHANGED",
      ACTION_SCHEDULE_EXACT_ALARM_PERMISSION_STATE_CHANGED =
          "android.app.action.SCHEDULE_EXACT_ALARM_PERMISSION_STATE_CHANGED";

  public interface OnAlarmListener { void onAlarm(); }

  public static final class AlarmClockInfo implements Parcelable {
    private final long triggerTime;
    private final PendingIntent showIntent;
    public AlarmClockInfo(long triggerTime, PendingIntent showIntent) {
      this.triggerTime = triggerTime;
      this.showIntent = showIntent;
    }
    public long getTriggerTime() { return triggerTime; }
    public PendingIntent getShowIntent() { return showIntent; }
    public int describeContents() { return 0; }
    public void writeToParcel(Parcel dest, int flags) { dest.writeParcelable(this, flags); }
    public static final Creator<AlarmClockInfo> CREATOR = new Creator<>() {
      public AlarmClockInfo createFromParcel(Parcel in) { return in.readParcelable(null); }
      public AlarmClockInfo[] newArray(int size) { return new AlarmClockInfo[size]; }
    };
  }

  private static final class Alarm implements Runnable {
    final PendingIntent operation;
    final OnAlarmListener listener;
    final Handler handler;
    final long interval;
    final AlarmClockInfo clock;
    long at;  // uptime
    Alarm(PendingIntent operation, OnAlarmListener listener, Handler handler, long at, long interval,
        AlarmClockInfo clock) {
      this.operation = operation; this.listener = listener; this.handler = handler;
      this.at = at; this.interval = interval; this.clock = clock;
    }
    @Override public void run() {
      synchronized (alarms) {
        if (!alarms.contains(this)) return;
        if (interval > 0) {
          // A repeating alarm that fell behind fires once and keeps its phase.
          at += interval * Math.max(1, (SystemClock.uptimeMillis() - at) / interval + 1);
          handler.postAtTime(this, this, at);
        } else alarms.remove(this);
      }
      if (listener != null) { listener.onAlarm(); return; }
      try {
        operation.send();
      } catch (PendingIntent.CanceledException e) {
        // The system drops an alarm whose PendingIntent was cancelled.
        remove(this);
      }
    }
  }
  private static final List<Alarm> alarms = new ArrayList<>();
  private static Handler main;

  private static Handler main() {
    synchronized (alarms) {
      if (main == null) main = new Handler(Looper.getMainLooper());
      return main;
    }
  }
  private static long uptime(int type, long trigger) {
    long now = switch (type) {
      case RTC_WAKEUP, RTC -> System.currentTimeMillis();
      case ELAPSED_REALTIME_WAKEUP, ELAPSED_REALTIME -> SystemClock.elapsedRealtime();
      default -> throw new IllegalArgumentException("Invalid alarm type " + type);
    };
    return SystemClock.uptimeMillis() + Math.max(0, trigger - now);
  }
  private static void remove(Alarm alarm) {
    synchronized (alarms) {
      alarms.remove(alarm);
      alarm.handler.removeCallbacksAndMessages(alarm);
    }
  }
  private static void cancelMatching(PendingIntent operation, OnAlarmListener listener) {
    synchronized (alarms) {
      for (Alarm a : new ArrayList<>(alarms))
        if ((operation != null && a.operation == operation) || (listener != null && a.listener == listener)) remove(a);
    }
  }
  // One alarm per PendingIntent or listener: setting it again replaces the earlier alarm.
  private static void schedule(int type, long trigger, long interval, PendingIntent operation,
      OnAlarmListener listener, Handler handler, AlarmClockInfo clock) {
    if (operation == null && listener == null) throw new NullPointerException("Alarm requires a PendingIntent or listener");
    Alarm alarm = new Alarm(operation, listener, handler != null ? handler : main(), uptime(type, trigger),
        Math.max(0, interval), clock);
    synchronized (alarms) {
      cancelMatching(operation, listener);
      alarms.add(alarm);
      alarm.handler.postAtTime(alarm, alarm, alarm.at);
    }
  }

  public void set(int type, long triggerAtMillis, PendingIntent operation) {
    schedule(type, triggerAtMillis, 0, operation, null, null, null);
  }
  public void set(int type, long triggerAtMillis, String tag, OnAlarmListener listener, Handler targetHandler) {
    schedule(type, triggerAtMillis, 0, null, listener, targetHandler, null);
  }
  public void setExact(int type, long triggerAtMillis, PendingIntent operation) {
    set(type, triggerAtMillis, operation);
  }
  public void setExact(int type, long triggerAtMillis, String tag, OnAlarmListener listener, Handler targetHandler) {
    set(type, triggerAtMillis, tag, listener, targetHandler);
  }
  public void setAndAllowWhileIdle(int type, long triggerAtMillis, PendingIntent operation) {
    set(type, triggerAtMillis, operation);
  }
  public void setExactAndAllowWhileIdle(int type, long triggerAtMillis, PendingIntent operation) {
    set(type, triggerAtMillis, operation);
  }
  public void setWindow(int type, long windowStartMillis, long windowLengthMillis, PendingIntent operation) {
    set(type, windowStartMillis, operation);
  }
  public void setWindow(int type, long windowStartMillis, long windowLengthMillis, String tag,
      OnAlarmListener listener, Handler targetHandler) {
    set(type, windowStartMillis, tag, listener, targetHandler);
  }
  public void setRepeating(int type, long triggerAtMillis, long intervalMillis, PendingIntent operation) {
    schedule(type, triggerAtMillis, intervalMillis, operation, null, null, null);
  }
  public void setInexactRepeating(int type, long triggerAtMillis, long intervalMillis, PendingIntent operation) {
    setRepeating(type, triggerAtMillis, intervalMillis, operation);
  }
  public void setAlarmClock(AlarmClockInfo info, PendingIntent operation) {
    schedule(RTC_WAKEUP, info.getTriggerTime(), 0, operation, null, null, info);
  }
  public void cancel(PendingIntent operation) {
    if (operation == null) throw new NullPointerException("cancel() called with a null PendingIntent");
    cancelMatching(operation, null);
  }
  public void cancel(OnAlarmListener listener) {
    if (listener == null) throw new NullPointerException("cancel() called with a null OnAlarmListener");
    cancelMatching(null, listener);
  }
  public void cancelAll() {
    synchronized (alarms) {
      for (Alarm a : new ArrayList<>(alarms)) remove(a);
    }
  }
  public boolean canScheduleExactAlarms() { return true; }
  public AlarmClockInfo getNextAlarmClock() {
    synchronized (alarms) {
      Alarm next = null;
      for (Alarm a : alarms) if (a.clock != null && (next == null || a.at < next.at)) next = a;
      return next == null ? null : next.clock;
    }
  }
}
