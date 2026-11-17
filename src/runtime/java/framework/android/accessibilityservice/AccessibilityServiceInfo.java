package android.accessibilityservice;

public class AccessibilityServiceInfo {
  public static final int FEEDBACK_ALL_MASK = -1, FEEDBACK_GENERIC = 16;
  public int feedbackType, flags, eventTypes;
  public String getId() { return null; }
  public android.content.pm.ResolveInfo getResolveInfo() { return null; }
  public int getCapabilities() { return 0; }
}
