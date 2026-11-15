package android.graphics;

import java.awt.geom.AffineTransform;

// AOSP graphics/java/android/graphics/Matrix.java, for the affine subset Canvas
// and Path apply (no perspective). pre* applies before the current matrix,
// post* after it, as in Skia.
public class Matrix {
  public static final int MSCALE_X = 0, MSKEW_X = 1, MTRANS_X = 2, MSKEW_Y = 3, MSCALE_Y = 4,
      MTRANS_Y = 5, MPERSP_0 = 6, MPERSP_1 = 7, MPERSP_2 = 8;
  private final AffineTransform transform = new AffineTransform();
  public Matrix() {}
  public Matrix(Matrix source) {
    if (source != null) set(source);
  }
  AffineTransform transform() {
    return transform;
  }
  public boolean isIdentity() {
    return transform.isIdentity();
  }
  public void reset() {
    transform.setToIdentity();
  }
  public void set(Matrix source) {
    if (source == null) reset();
    else transform.setTransform(source.transform);
  }
  public void setTranslate(float dx, float dy) {
    transform.setToTranslation(dx, dy);
  }
  public void setScale(float sx, float sy) {
    transform.setToScale(sx, sy);
  }
  public void setScale(float sx, float sy, float px, float py) {
    transform.setToTranslation(px, py);
    transform.scale(sx, sy);
    transform.translate(-px, -py);
  }
  public void setRotate(float degrees) {
    transform.setToRotation(Math.toRadians(degrees));
  }
  public void setRotate(float degrees, float px, float py) {
    transform.setToRotation(Math.toRadians(degrees), px, py);
  }
  public boolean preTranslate(float dx, float dy) {
    transform.translate(dx, dy);
    return true;
  }
  public boolean preScale(float sx, float sy) {
    transform.scale(sx, sy);
    return true;
  }
  public boolean preRotate(float degrees) {
    transform.rotate(Math.toRadians(degrees));
    return true;
  }
  public boolean preConcat(Matrix other) {
    transform.concatenate(other.transform);
    return true;
  }
  public boolean postTranslate(float dx, float dy) {
    transform.preConcatenate(AffineTransform.getTranslateInstance(dx, dy));
    return true;
  }
  public boolean postScale(float sx, float sy) {
    transform.preConcatenate(AffineTransform.getScaleInstance(sx, sy));
    return true;
  }
  public boolean postRotate(float degrees) {
    transform.preConcatenate(AffineTransform.getRotateInstance(Math.toRadians(degrees)));
    return true;
  }
  public boolean postConcat(Matrix other) {
    transform.preConcatenate(other.transform);
    return true;
  }
  // The nine values in Android's row-major order; the perspective row of the
  // affine subset is always 0, 0, 1, and setValues ignores it.
  public void getValues(float[] values) {
    if (values.length < 9) throw new ArrayIndexOutOfBoundsException();
    values[MSCALE_X] = (float) transform.getScaleX();
    values[MSKEW_X] = (float) transform.getShearX();
    values[MTRANS_X] = (float) transform.getTranslateX();
    values[MSKEW_Y] = (float) transform.getShearY();
    values[MSCALE_Y] = (float) transform.getScaleY();
    values[MTRANS_Y] = (float) transform.getTranslateY();
    values[MPERSP_0] = 0;
    values[MPERSP_1] = 0;
    values[MPERSP_2] = 1;
  }
  public void setValues(float[] values) {
    if (values.length < 9) throw new ArrayIndexOutOfBoundsException();
    transform.setTransform(values[MSCALE_X], values[MSKEW_Y], values[MSKEW_X], values[MSCALE_Y],
        values[MTRANS_X], values[MTRANS_Y]);
  }
  public void mapPoints(float[] points) {
    transform.transform(points, 0, points, 0, points.length / 2);
  }
  @Override public boolean equals(Object o) {
    return o instanceof Matrix m && m.transform.equals(transform);
  }
  @Override public int hashCode() {
    return transform.hashCode();
  }
}
