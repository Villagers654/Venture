package android.widget;
public class RadioButton extends CompoundButton {
  public RadioButton(android.content.Context context) { this(context, null); }
  public RadioButton(android.content.Context context, android.util.AttributeSet attributes) {
    this(context, attributes, 0);
  }
  public RadioButton(android.content.Context context, android.util.AttributeSet attributes, int style) {
    this(context, attributes, style, 0);
  }
  public RadioButton(android.content.Context context, android.util.AttributeSet attributes, int style, int resource) {
    super(context, attributes, style, resource);
  }
  // AOSP: a checked radio button stays checked when toggled.
  public void toggle() {
    if (!isChecked()) super.toggle();
  }
}