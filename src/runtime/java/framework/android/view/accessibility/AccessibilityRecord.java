package android.view.accessibility;
public class AccessibilityRecord {
  private CharSequence className, description;
  private final java.util.List<CharSequence> text = new java.util.ArrayList<>();
  public void setClassName(CharSequence n) {
    className = n;
  }
  public CharSequence getClassName() {
    return className;
  }
  public void setContentDescription(CharSequence d) {
    description = d;
  }
  public CharSequence getContentDescription() {
    return description;
  }
  public java.util.List<CharSequence> getText() {
    return text;
  }
}
