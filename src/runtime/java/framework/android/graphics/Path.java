package android.graphics;

import java.awt.geom.*;

// AOSP graphics/java/android/graphics/Path.java: contours of lines and curves,
// kept as Java2D geometry that Canvas fills or strokes into a Bitmap.
public class Path {
  public enum Direction { CW, CCW }
  public enum FillType { WINDING, EVEN_ODD, INVERSE_WINDING, INVERSE_EVEN_ODD }
  private final Path2D.Float shape = new Path2D.Float();
  private FillType fill = FillType.WINDING;
  public Path() {}
  public Path(Path source) {
    if (source != null) set(source);
  }
  java.awt.Shape shape() {
    return shape;
  }
  public void set(Path source) {
    shape.reset();
    shape.append(source.shape, false);
    setFillType(source.fill);
  }
  public void reset() {
    shape.reset();
    setFillType(FillType.WINDING);
  }
  public void rewind() {
    shape.reset();
  }
  public boolean isEmpty() {
    return shape.getCurrentPoint() == null;
  }
  public FillType getFillType() {
    return fill;
  }
  public void setFillType(FillType type) {
    fill = type;
    shape.setWindingRule(type == FillType.EVEN_ODD || type == FillType.INVERSE_EVEN_ODD
        ? Path2D.WIND_EVEN_ODD : Path2D.WIND_NON_ZERO);
  }
  // AOSP: a drawing call without a preceding moveTo starts at (0,0).
  private void start() {
    if (shape.getCurrentPoint() == null) shape.moveTo(0, 0);
  }
  public void moveTo(float x, float y) {
    shape.moveTo(x, y);
  }
  public void rMoveTo(float dx, float dy) {
    Point2D p = shape.getCurrentPoint();
    shape.moveTo((p == null ? 0 : p.getX()) + dx, (p == null ? 0 : p.getY()) + dy);
  }
  public void lineTo(float x, float y) {
    start();
    shape.lineTo(x, y);
  }
  public void rLineTo(float dx, float dy) {
    start();
    Point2D p = shape.getCurrentPoint();
    shape.lineTo(p.getX() + dx, p.getY() + dy);
  }
  public void quadTo(float x1, float y1, float x2, float y2) {
    start();
    shape.quadTo(x1, y1, x2, y2);
  }
  public void cubicTo(float x1, float y1, float x2, float y2, float x3, float y3) {
    start();
    shape.curveTo(x1, y1, x2, y2, x3, y3);
  }
  public void close() {
    if (shape.getCurrentPoint() != null) shape.closePath();
  }
  public void addRect(float left, float top, float right, float bottom, Direction dir) {
    shape.append(new Rectangle2D.Float(left, top, right - left, bottom - top), false);
  }
  public void addRect(RectF rect, Direction dir) {
    addRect(rect.left, rect.top, rect.right, rect.bottom, dir);
  }
  public void addOval(float left, float top, float right, float bottom, Direction dir) {
    shape.append(new Ellipse2D.Float(left, top, right - left, bottom - top), false);
  }
  public void addOval(RectF oval, Direction dir) {
    addOval(oval.left, oval.top, oval.right, oval.bottom, dir);
  }
  public void addCircle(float x, float y, float radius, Direction dir) {
    addOval(x - radius, y - radius, x + radius, y + radius, dir);
  }
  public void addRoundRect(RectF rect, float rx, float ry, Direction dir) {
    shape.append(new RoundRectangle2D.Float(rect.left, rect.top, rect.right - rect.left,
        rect.bottom - rect.top, 2 * rx, 2 * ry), false);
  }
  public void addPath(Path source) {
    shape.append(source.shape, false);
  }
  public void transform(Matrix matrix) {
    shape.transform(matrix.transform());
  }
  public void offset(float dx, float dy) {
    shape.transform(AffineTransform.getTranslateInstance(dx, dy));
  }
  public void computeBounds(RectF bounds, boolean exact) {
    if (isEmpty()) {
      bounds.set(0, 0, 0, 0);
      return;
    }
    Rectangle2D r = shape.getBounds2D();
    bounds.set((float) r.getMinX(), (float) r.getMinY(), (float) r.getMaxX(), (float) r.getMaxY());
  }
}
