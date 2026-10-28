package android.widget;
public abstract class CompoundButton extends Button implements Checkable {
  public interface OnCheckedChangeListener {
    void onCheckedChanged(CompoundButton button, boolean checked);
  }
  private boolean checked, notifying;
  private OnCheckedChangeListener listener;
  public CompoundButton(android.content.Context context) { this(context, null); }
  public CompoundButton(android.content.Context context, android.util.AttributeSet attributes) { this(context, attributes, 0); }
  public CompoundButton(android.content.Context context, android.util.AttributeSet attributes, int style) {
    this(context, attributes, style, 0);
  }
  public CompoundButton(android.content.Context context, android.util.AttributeSet attributes, int style, int resource) {
    super(context, attributes);
    setClickable(true);
    // AOSP CompoundButton constructor: android.R.attr.checked, including theme/style resolution.
    var values = context.obtainStyledAttributes(attributes, new int[]{0x01010106}, style, resource);
    try { setChecked(values.getBoolean(0, false)); }
    finally { values.recycle(); }
  }
  public boolean isChecked() { return checked; }
  public void setChecked(boolean value) {
    if (checked == value) return;
    checked = value;
    invalidate();
    if (notifying || listener == null) return;
    notifying = true;
    try { listener.onCheckedChanged(this, checked); }
    finally { notifying = false; }
  }
  public void toggle() { setChecked(!checked); }
  public void setOnCheckedChangeListener(OnCheckedChangeListener value) { listener = value; }
  @Override public boolean performClick() { toggle(); return super.performClick(); }
}
