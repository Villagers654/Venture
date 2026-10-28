package android.widget;

import android.content.Context;
import android.graphics.drawable.Drawable;
import android.util.AttributeSet;

/** Drawable state for image widgets; host Java view composition is unavailable. */
public class ImageView extends android.view.View {
  private Drawable drawable;
  private boolean compositionReported;
  public ImageView(Context context) { super(context); }
  public ImageView(Context context, AttributeSet attrs) { super(context, attrs); }
  public ImageView(Context context, AttributeSet attrs, int style) { super(context, attrs, style); }
  public ImageView(Context context, AttributeSet attrs, int style, int resource) {
    super(context, attrs, style, resource);
  }
  public Drawable getDrawable() { return drawable; }
  public void setImageDrawable(Drawable value) {
    if (drawable == value) return;
    drawable = value;
    requestLayout();
    invalidate();
    if (value != null && !compositionReported) {
      compositionReported = true;
      android.util.Log.w("ImageView", "Image composition unavailable: no host Java view renderer");
    }
  }
  // AOSP core/java/android/widget/ImageView.java: resource zero clears the image.
  public void setImageResource(int id) {
    setImageDrawable(id == 0 ? null : getResources().getDrawable(id));
  }
  public CharSequence getAccessibilityClassName() { return ImageView.class.getName(); }
}
