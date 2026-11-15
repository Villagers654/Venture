package android.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Matrix;
import android.graphics.RectF;
import android.graphics.drawable.BitmapDrawable;
import android.graphics.drawable.Drawable;
import android.os.Bundle;
import android.os.Parcelable;
import android.view.View;
import android.widget.ImageView;
import java.util.List;
import java.util.Map;

/**
 * AOSP core/java/android/app/SharedElementCallback.java: every callback is a no-op by default.
 * androidx.core's ActivityCompat wraps its own callback in a subclass of this one, so
 * verifying ActivityCompat loads it.
 */
public abstract class SharedElementCallback {
  private static final String BUNDLE_SNAPSHOT_BITMAP = "sharedElement:snapshot:bitmap";
  private static final String BUNDLE_SNAPSHOT_IMAGE_SCALETYPE = "sharedElement:snapshot:imageScaleType";
  private static final String BUNDLE_SNAPSHOT_IMAGE_MATRIX = "sharedElement:snapshot:imageMatrix";
  // AOSP TransitionUtils.MAX_IMAGE_SIZE: snapshots are scaled to at most this many pixels.
  private static final int MAX_IMAGE_SIZE = 1024 * 1024;
  // AOSP SharedElementCallback.NULL_CALLBACK: what a null callback is replaced with.
  static final SharedElementCallback NULL_CALLBACK = new SharedElementCallback() {};

  public void onSharedElementStart(List<String> sharedElementNames, List<View> sharedElements,
      List<View> sharedElementSnapshots) {}
  public void onSharedElementEnd(List<String> sharedElementNames, List<View> sharedElements,
      List<View> sharedElementSnapshots) {}
  public void onRejectSharedElements(List<View> rejectedSharedElements) {}
  public void onMapSharedElements(List<String> names, Map<String, View> sharedElements) {}

  /**
   * AOSP: an ImageView with a drawable and no visible background is captured as a Bundle with
   * its image, scale type and (for MATRIX) image matrix. Any other view is rendered into a
   * bitmap; Venture has no host renderer for Java views, so there is no such snapshot.
   */
  public Parcelable onCaptureSharedElementSnapshot(View sharedElement, Matrix viewToGlobalMatrix,
      RectF screenBounds) {
    if (sharedElement instanceof ImageView imageView) {
      Drawable d = imageView.getDrawable();
      Drawable bg = imageView.getBackground();
      if (d != null && (bg == null || bg.getAlpha() == 0)) {
        Bitmap bitmap = createDrawableBitmap(d);
        if (bitmap != null) {
          Bundle bundle = new Bundle();
          bundle.putParcelable(BUNDLE_SNAPSHOT_BITMAP, bitmap);
          bundle.putString(BUNDLE_SNAPSHOT_IMAGE_SCALETYPE, imageView.getScaleType().toString());
          if (imageView.getScaleType() == ImageView.ScaleType.MATRIX) {
            float[] values = new float[9];
            imageView.getImageMatrix().getValues(values);
            bundle.putFloatArray(BUNDLE_SNAPSHOT_IMAGE_MATRIX, values);
          }
          return bundle;
        }
      }
    }
    return null;
  }

  /**
   * AOSP: a Bundle snapshot becomes an ImageView with the captured image, scale type and image
   * matrix; a Bitmap snapshot becomes a View with that bitmap as its background.
   */
  public View onCreateSnapshotView(Context context, Parcelable snapshot) {
    View view = null;
    if (snapshot instanceof Bundle bundle) {
      Bitmap bitmap = bundle.getParcelable(BUNDLE_SNAPSHOT_BITMAP);
      if (bitmap == null) return null;
      ImageView imageView = new ImageView(context);
      view = imageView;
      imageView.setImageBitmap(bitmap);
      imageView.setScaleType(ImageView.ScaleType.valueOf(bundle.getString(BUNDLE_SNAPSHOT_IMAGE_SCALETYPE)));
      if (imageView.getScaleType() == ImageView.ScaleType.MATRIX) {
        float[] values = bundle.getFloatArray(BUNDLE_SNAPSHOT_IMAGE_MATRIX);
        Matrix matrix = new Matrix();
        matrix.setValues(values);
        imageView.setImageMatrix(matrix);
      }
    } else if (snapshot instanceof Bitmap bitmap) {
      view = new View(context);
      view.setBackground(new BitmapDrawable(context.getResources(), bitmap));
    }
    return view;
  }

  public void onSharedElementsArrived(List<String> sharedElementNames, List<View> sharedElements,
      OnSharedElementsReadyListener listener) {
    listener.onSharedElementsReady();
  }

  public interface OnSharedElementsReadyListener {
    void onSharedElementsReady();
  }

  // AOSP TransitionUtils.createDrawableBitmap, drawing in software instead of a RenderNode.
  private static Bitmap createDrawableBitmap(Drawable drawable) {
    int width = drawable.getIntrinsicWidth();
    int height = drawable.getIntrinsicHeight();
    if (width <= 0 || height <= 0) return null;
    float scale = Math.min(1f, ((float) MAX_IMAGE_SIZE) / (width * height));
    if (drawable instanceof BitmapDrawable bitmapDrawable && scale == 1f)
      return bitmapDrawable.getBitmap();
    int bitmapWidth = (int) (width * scale);
    int bitmapHeight = (int) (height * scale);
    Bitmap bitmap = Bitmap.createBitmap(bitmapWidth, bitmapHeight, Bitmap.Config.ARGB_8888);
    var existing = drawable.copyBounds();
    drawable.setBounds(0, 0, bitmapWidth, bitmapHeight);
    drawable.draw(new Canvas(bitmap));
    drawable.setBounds(existing);
    return bitmap;
  }
}
