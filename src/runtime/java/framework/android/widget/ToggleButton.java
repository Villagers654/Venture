package android.widget;
public class ToggleButton extends CompoundButton {
  private CharSequence textOn = "ON", textOff = "OFF";
  public ToggleButton(android.content.Context context) { this(context, null); }
  public ToggleButton(android.content.Context context, android.util.AttributeSet attributes) { this(context, attributes, 0); }
  public ToggleButton(android.content.Context context, android.util.AttributeSet attributes, int style) { this(context, attributes, style, 0); }
  public ToggleButton(android.content.Context context, android.util.AttributeSet attributes, int style, int resource) {
    super(context, attributes, style, resource);
  }
  public CharSequence getTextOn() { return textOn; }
  public void setTextOn(CharSequence t) { textOn = t; }
  public CharSequence getTextOff() { return textOff; }
  public void setTextOff(CharSequence t) { textOff = t; }
}