package com.oculus.os;

/** Event contract used by the device telemetry client. Collection is disabled. */
public class AnalyticsEvent {
  public AnalyticsEvent(String name) {}

  public AnalyticsEvent setExtra(String key, Object value) {
    // Do not retain or serialize payloads when collection is disabled.
    return this;
  }
}
