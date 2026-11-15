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
  // AOSP ImageView.ScaleType, with its native ordinals.
  public enum ScaleType {
    MATRIX(0), FIT_XY(1), FIT_START(2), FIT_CENTER(3), FIT_END(4), CENTER(5), CENTER_CROP(6), CENTER_INSIDE(7);
    ScaleType(int ni) { nativeInt = ni; }
    final int nativeInt;
  }
  private ScaleType scaleType = ScaleType.FIT_CENTER;
  private final android.graphics.Matrix matrix = new android.graphics.Matrix();
  public void setScaleType(ScaleType value) {
    if (value == null) throw new NullPointerException();
    if (scaleType != value) { scaleType = value; requestLayout(); invalidate(); }
  }
  public ScaleType getScaleType() { return scaleType; }
  // AOSP: an identity matrix is stored as no matrix.
  public void setImageMatrix(android.graphics.Matrix value) {
    if (value != null && value.isIdentity()) value = null;
    if (value == null ? !matrix.isIdentity() : !matrix.equals(value)) { matrix.set(value); invalidate(); }
  }
  // The draw matrix: the image matrix for MATRIX. The fitted scale types derive
  // theirs from composing the image into the view's bounds, which no host Java
  // view renderer does here, so they report identity.
  public android.graphics.Matrix getImageMatrix() {
    return scaleType == ScaleType.MATRIX && !matrix.isIdentity() ? matrix : new android.graphics.Matrix();
  }
  public void setImageBitmap(android.graphics.Bitmap bitmap) {
    setImageDrawable(new android.graphics.drawable.BitmapDrawable(getContext().getResources(), bitmap));
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
