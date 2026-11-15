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
  // Java2D rectangles, ellipses and round rectangles run clockwise (y down), as
  // Skia's CW contours do; CCW appends them reversed, so winding fills cut holes.
  private void append(java.awt.Shape source, Direction dir) {
    if (dir != Direction.CCW) {
      shape.append(source, false);
      return;
    }
    var segments = new java.util.ArrayList<float[]>();
    float[] c = new float[6];
    float startX = 0, startY = 0, x = 0, y = 0;
    boolean open = false, closed = false;
    for (PathIterator it = source.getPathIterator(null); !it.isDone(); it.next()) {
      int type = it.currentSegment(c);
      if (type == PathIterator.SEG_MOVETO) {
        if (open) appendReversed(segments, startX, startY, closed);
        segments.clear();
        open = true;
        closed = false;
        startX = x = c[0];
        startY = y = c[1];
      } else if (type == PathIterator.SEG_CLOSE) {
        closed = true;
        x = startX;
        y = startY;
      } else {
        int n = type == PathIterator.SEG_LINETO ? 2 : type == PathIterator.SEG_QUADTO ? 4 : 6;
        float[] segment = new float[2 + n];
        segment[0] = x;
        segment[1] = y;
        System.arraycopy(c, 0, segment, 2, n);
        segments.add(segment);
        x = c[n - 2];
        y = c[n - 1];
      }
    }
    if (open) appendReversed(segments, startX, startY, closed);
  }
  private void appendReversed(java.util.List<float[]> segments, float startX, float startY, boolean closed) {
    if (segments.isEmpty()) {
      shape.moveTo(startX, startY);
      if (closed) shape.closePath();
      return;
    }
    float[] last = segments.get(segments.size() - 1);
    shape.moveTo(last[last.length - 2], last[last.length - 1]);
    for (int i = segments.size() - 1; i >= 0; i--) {
      float[] e = segments.get(i);
      switch (e.length) {
        case 4 -> shape.lineTo(e[0], e[1]);
        case 6 -> shape.quadTo(e[2], e[3], e[0], e[1]);
        default -> shape.curveTo(e[4], e[5], e[2], e[3], e[0], e[1]);
      }
    }
    if (closed) shape.closePath();
  }
  public void addRect(float left, float top, float right, float bottom, Direction dir) {
    append(new Rectangle2D.Float(left, top, right - left, bottom - top), dir);
  }
  public void addRect(RectF rect, Direction dir) {
    addRect(rect.left, rect.top, rect.right, rect.bottom, dir);
  }
  public void addOval(float left, float top, float right, float bottom, Direction dir) {
    append(new Ellipse2D.Float(left, top, right - left, bottom - top), dir);
  }
  public void addOval(RectF oval, Direction dir) {
    addOval(oval.left, oval.top, oval.right, oval.bottom, dir);
  }
  public void addCircle(float x, float y, float radius, Direction dir) {
    addOval(x - radius, y - radius, x + radius, y + radius, dir);
  }
  public void addRoundRect(RectF rect, float rx, float ry, Direction dir) {
    addRoundRect(rect.left, rect.top, rect.right, rect.bottom, rx, ry, dir);
  }
  public void addRoundRect(float left, float top, float right, float bottom, float rx, float ry, Direction dir) {
    addRoundRect(left, top, right, bottom, new float[] {rx, ry, rx, ry, rx, ry, rx, ry}, dir);
  }
  public void addRoundRect(RectF rect, float[] radii, Direction dir) {
    if (rect == null) throw new NullPointerException("need rect parameter");
    addRoundRect(rect.left, rect.top, rect.right, rect.bottom, radii, dir);
  }
  // Corner radii are top-left, top-right, bottom-right, bottom-left (x, y) pairs.
  // As SkRRect: invalid radii become 0, a corner with one zero radius is square, and
  // radii that overflow a side all scale down together.
  public void addRoundRect(float left, float top, float right, float bottom, float[] radii, Direction dir) {
    if (radii.length < 8) throw new ArrayIndexOutOfBoundsException("radii[] needs 8 values");
    float w = right - left, h = bottom - top;
    if (!(w > 0 && h > 0)) {
      addRect(left, top, right, bottom, dir);
      return;
    }
    float[] r = new float[8];
    for (int i = 0; i < 8; i += 2) {
      float x = radii[i], y = radii[i + 1];
      if (!(x > 0 && y > 0) || !Float.isFinite(x) || !Float.isFinite(y)) x = y = 0;
      r[i] = x;
      r[i + 1] = y;
    }
    double scale = Math.min(Math.min(fit(w, r[0], r[2]), fit(h, r[3], r[5])), Math.min(fit(w, r[4], r[6]), fit(h, r[7], r[1])));
    if (scale < 1)
      for (int i = 0; i < 8; i++) r[i] = (float) (r[i] * scale);
    final float k = 1 - 0.5522847498f;
    var path = new Path2D.Float();
    path.moveTo(left + r[0], top);
    path.lineTo(right - r[2], top);
    path.curveTo(right - r[2] * k, top, right, top + r[3] * k, right, top + r[3]);
    path.lineTo(right, bottom - r[5]);
    path.curveTo(right, bottom - r[5] * k, right - r[4] * k, bottom, right - r[4], bottom);
    path.lineTo(left + r[6], bottom);
    path.curveTo(left + r[6] * k, bottom, left, bottom - r[7] * k, left, bottom - r[7]);
    path.lineTo(left, top + r[1]);
    path.curveTo(left, top + r[1] * k, left + r[0] * k, top, left + r[0], top);
    path.closePath();
    append(path, dir);
  }
  private static double fit(float side, float a, float b) {
    return a + b > side ? side / ((double) a + b) : 1;
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
