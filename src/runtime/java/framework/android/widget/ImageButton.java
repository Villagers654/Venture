package android.widget;

import android.content.Context;
import android.util.AttributeSet;

// AOSP core/java/android/widget/ImageButton.java: ImageView with button focus.
public class ImageButton extends ImageView {
  public ImageButton(Context context) { this(context, null); }
  public ImageButton(Context context, AttributeSet attrs) { this(context, attrs, 0); }
  public ImageButton(Context context, AttributeSet attrs, int style) { this(context, attrs, style, 0); }
  public ImageButton(Context context, AttributeSet attrs, int style, int resource) {
    super(context, attrs, style, resource);
    setFocusable(true);
  }
  public CharSequence getAccessibilityClassName() { return ImageButton.class.getName(); }
}
