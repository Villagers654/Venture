package android.view;

// API 24 per-frame timing record delivered to Window.OnFrameMetricsAvailableListener.
public final class FrameMetrics {
  public static final int UNKNOWN_DELAY_DURATION = 0, INPUT_HANDLING_DURATION = 1,
      ANIMATION_DURATION = 2, LAYOUT_MEASURE_DURATION = 3, DRAW_DURATION = 4, SYNC_DURATION = 5,
      COMMAND_ISSUE_DURATION = 6, SWAP_BUFFERS_DURATION = 7, TOTAL_DURATION = 8,
      FIRST_DRAW_FRAME = 9, INTENDED_VSYNC_TIMESTAMP = 10, VSYNC_TIMESTAMP = 11,
      GPU_DURATION = 12, DEADLINE = 13;
  private final long[] metrics;
  public FrameMetrics(FrameMetrics other) {
    metrics = other.metrics.clone();
  }
  FrameMetrics(long[] metrics) {
    this.metrics = metrics.clone();
  }
  // AOSP returns -1 for an id it does not know.
  public long getMetric(int id) {
    return id >= 0 && id < metrics.length ? metrics[id] : -1;
  }
}
