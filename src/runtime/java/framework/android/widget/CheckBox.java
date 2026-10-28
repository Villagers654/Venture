package android.widget;

/** AOSP CheckBox delegates checked state and click toggling to CompoundButton. */
public class CheckBox extends CompoundButton {
  public CheckBox(android.content.Context context) { this(context, null); }
  public CheckBox(android.content.Context context, android.util.AttributeSet attributes) {
    this(context, attributes, 0x0101006c); // android.R.attr.checkboxStyle
  }
  public CheckBox(android.content.Context context, android.util.AttributeSet attributes, int style) {
    this(context, attributes, style, 0);
  }
  public CheckBox(android.content.Context context, android.util.AttributeSet attributes, int style, int resource) {
    super(context, attributes, style, resource);
  }
  public CharSequence getAccessibilityClassName() { return CheckBox.class.getName(); }
}
