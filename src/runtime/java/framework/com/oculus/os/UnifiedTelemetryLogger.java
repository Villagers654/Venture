package com.oculus.os;

import android.content.Context;

/** Device telemetry entry point with collection and transmission disabled. */
public class UnifiedTelemetryLogger {
  private static final UnifiedTelemetryLogger INSTANCE = new UnifiedTelemetryLogger();

  private UnifiedTelemetryLogger() {}

  public static UnifiedTelemetryLogger getInstance(Context context) {
    return INSTANCE;
  }

  public void reportEvent(AnalyticsEvent event, boolean immediate) {
    // Disabled telemetry accepts events without retaining data or contacting a service.
  }
}
