package android.graphics;

import java.awt.*;
import java.awt.geom.*;
import java.util.ArrayDeque;

// A canvas over a Bitmap draws into its pixels with Java2D (apps render text and
// shapes into bitmaps they upload as textures). There is no host view renderer, so
// a canvas without a bitmap accepts drawing and discards it, keeping its matrix,
// clip and save depth.
public class Canvas {
  private Bitmap bitmap;
  private int width, height;
  private AffineTransform matrix = new AffineTransform();
  private Shape clip;  // device space; null = unclipped
  private record State(AffineTransform matrix, Shape clip) {}
  private final ArrayDeque<State> saved = new ArrayDeque<>();
  public Canvas() {}
  public Canvas(Bitmap bitmap) {
    setBitmap(bitmap);
  }
  // AOSP: setBitmap resets the matrix and clip.
  public void setBitmap(Bitmap bitmap) {
    if (bitmap != null && !bitmap.isMutable())
      throw new IllegalStateException("Immutable bitmap passed to Canvas constructor");
    this.bitmap = bitmap;
    width = bitmap == null ? 0 : bitmap.getWidth();
    height = bitmap == null ? 0 : bitmap.getHeight();
    matrix = new AffineTransform();
    clip = null;
  }
  public int getWidth() {
    return width;
  }
  public int getHeight() {
    return height;
  }
  public boolean isHardwareAccelerated() {
    return false;
  }
  public int save() {
    saved.push(new State(new AffineTransform(matrix), clip));
    return saved.size();
  }
  public int getSaveCount() {
    return saved.size() + 1;
  }
  public void restore() {
    if (saved.isEmpty()) throw new IllegalStateException("Underflow in restore");
    State state = saved.pop();
    matrix = state.matrix();
    clip = state.clip();
  }
  public void restoreToCount(int count) {
    if (count < 1 || count > getSaveCount()) throw new IllegalArgumentException("Bad save count " + count);
    while (getSaveCount() > count) restore();
  }
  public void translate(float dx, float dy) { matrix.translate(dx, dy); }
  public void scale(float sx, float sy) { matrix.scale(sx, sy); }
  public void rotate(float degrees) { matrix.rotate(Math.toRadians(degrees)); }
  public void concat(Matrix m) {
    if (m != null) matrix.concatenate(m.transform());
  }
  public void setMatrix(Matrix m) {
    matrix = m == null ? new AffineTransform() : new AffineTransform(m.transform());
  }
  public void getMatrix(Matrix out) {
    out.transform().setTransform(matrix);
  }
  private boolean clip(Shape local) {
    Area area = new Area(matrix.createTransformedShape(local));
    if (clip != null) area.intersect(new Area(clip));
    clip = area;
    return !area.isEmpty();
  }
  public boolean clipRect(Rect rect) {
    return clipRect(rect.left, rect.top, rect.right, rect.bottom);
  }
  public boolean clipRect(RectF rect) {
    return clipRect(rect.left, rect.top, rect.right, rect.bottom);
  }
  public boolean clipRect(float left, float top, float right, float bottom) {
    return clip(new Rectangle2D.Float(left, top, right - left, bottom - top));
  }
  public boolean clipRect(int left, int top, int right, int bottom) {
    return clipRect((float) left, top, right, bottom);
  }
  public boolean clipPath(Path path) {
    return clip(path.shape());
  }
  // A Graphics2D for one drawing call, or null without a bitmap.
  private Graphics2D graphics(Paint paint) {
    if (bitmap == null) return null;
    if (bitmap.isRecycled()) throw new RuntimeException("Canvas: trying to use a recycled bitmap");
    Graphics2D g = bitmap.image.createGraphics();
    if (clip != null) g.setClip(clip);
    g.transform(matrix);
    if (paint != null) {
      boolean smooth = paint.isAntiAlias();
      g.setRenderingHint(RenderingHints.KEY_ANTIALIASING,
          smooth ? RenderingHints.VALUE_ANTIALIAS_ON : RenderingHints.VALUE_ANTIALIAS_OFF);
      g.setRenderingHint(RenderingHints.KEY_TEXT_ANTIALIASING,
          smooth ? RenderingHints.VALUE_TEXT_ANTIALIAS_ON : RenderingHints.VALUE_TEXT_ANTIALIAS_OFF);
      g.setRenderingHint(RenderingHints.KEY_FRACTIONALMETRICS, (paint.getFlags() & Paint.SUBPIXEL_TEXT_FLAG) != 0
          ? RenderingHints.VALUE_FRACTIONALMETRICS_ON : RenderingHints.VALUE_FRACTIONALMETRICS_OFF);
      g.setRenderingHint(RenderingHints.KEY_INTERPOLATION, (paint.getFlags() & Paint.FILTER_BITMAP_FLAG) != 0
          ? RenderingHints.VALUE_INTERPOLATION_BILINEAR : RenderingHints.VALUE_INTERPOLATION_NEAREST_NEIGHBOR);
      g.setColor(new java.awt.Color(paint.getColor(), true));
    }
    return g;
  }
  private static BasicStroke stroke(Paint paint) {
    int cap = switch (paint.getStrokeCap()) {
      case ROUND -> BasicStroke.CAP_ROUND;
      case SQUARE -> BasicStroke.CAP_SQUARE;
      default -> BasicStroke.CAP_BUTT;
    };
    int join = switch (paint.getStrokeJoin()) {
      case ROUND -> BasicStroke.JOIN_ROUND;
      case BEVEL -> BasicStroke.JOIN_BEVEL;
      default -> BasicStroke.JOIN_MITER;
    };
    // Skia's zero width is a one-pixel hairline.
    return new BasicStroke(Math.max(paint.getStrokeWidth(), 0f), cap, join);
  }
  private void draw(Shape shape, Paint paint) {
    Graphics2D g = graphics(paint);
    if (g == null) return;
    try {
      Paint.Style style = paint.getStyle();
      if (style != Paint.Style.STROKE) g.fill(shape);
      if (style != Paint.Style.FILL) {
        g.setStroke(stroke(paint));
        g.draw(shape);
      }
    } finally {
      g.dispose();
    }
  }
  public void drawColor(int color) {
    drawColor(color, PorterDuff.Mode.SRC_OVER);
  }
  public void drawColor(int color, PorterDuff.Mode mode) {
    Graphics2D g = graphics(null);
    if (g == null) return;
    try {
      g.setTransform(new AffineTransform());
      if (mode == PorterDuff.Mode.CLEAR || mode == PorterDuff.Mode.SRC)
        g.setComposite(AlphaComposite.Src);
      g.setColor(new java.awt.Color(mode == PorterDuff.Mode.CLEAR ? 0 : color, true));
      g.fillRect(0, 0, width, height);
    } finally {
      g.dispose();
    }
  }
  public void drawARGB(int a, int r, int g, int b) {
    drawColor((a << 24) | (r << 16) | (g << 8) | b);
  }
  public void drawRGB(int r, int g, int b) {
    drawARGB(255, r, g, b);
  }
  public void drawPaint(Paint paint) {
    Graphics2D g = graphics(paint);
    if (g == null) return;
    try {
      g.setTransform(new AffineTransform());
      g.fillRect(0, 0, width, height);
    } finally {
      g.dispose();
    }
  }
  public void drawRect(Rect rect, Paint paint) {
    drawRect(rect.left, rect.top, rect.right, rect.bottom, paint);
  }
  public void drawRect(RectF rect, Paint paint) {
    drawRect(rect.left, rect.top, rect.right, rect.bottom, paint);
  }
  public void drawRect(float left, float top, float right, float bottom, Paint paint) {
    draw(new Rectangle2D.Float(left, top, right - left, bottom - top), paint);
  }
  public void drawRoundRect(RectF rect, float rx, float ry, Paint paint) {
    draw(new RoundRectangle2D.Float(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
        2 * rx, 2 * ry), paint);
  }
  public void drawOval(RectF oval, Paint paint) {
    draw(new Ellipse2D.Float(oval.left, oval.top, oval.right - oval.left, oval.bottom - oval.top), paint);
  }
  public void drawCircle(float cx, float cy, float radius, Paint paint) {
    draw(new Ellipse2D.Float(cx - radius, cy - radius, 2 * radius, 2 * radius), paint);
  }
  public void drawPath(Path path, Paint paint) {
    draw(path.shape(), paint);
  }
  public void drawLine(float startX, float startY, float stopX, float stopY, Paint paint) {
    Graphics2D g = graphics(paint);
    if (g == null) return;
    try {
      g.setStroke(stroke(paint));
      g.draw(new Line2D.Float(startX, startY, stopX, stopY));
    } finally {
      g.dispose();
    }
  }
  private void image(Bitmap source, Rect src, RectF dst, Paint paint) {
    Graphics2D g = graphics(paint);
    if (g == null) return;
    try {
      if (paint != null && paint.getAlpha() < 255)
        g.setComposite(AlphaComposite.getInstance(AlphaComposite.SRC_OVER, paint.getAlpha() / 255f));
      int sx = src == null ? 0 : src.left, sy = src == null ? 0 : src.top;
      int sw = src == null ? source.getWidth() : src.right - src.left;
      int sh = src == null ? source.getHeight() : src.bottom - src.top;
      var at = new AffineTransform();
      at.translate(dst.left, dst.top);
      at.scale((dst.right - dst.left) / sw, (dst.bottom - dst.top) / sh);
      at.translate(-sx, -sy);
      Graphics2D target = (Graphics2D) g.create();
      try {
        target.transform(at);
        target.clipRect(sx, sy, sw, sh);
        target.drawImage(source.image, 0, 0, null);
      } finally {
        target.dispose();
      }
    } finally {
      g.dispose();
    }
  }
  public void drawBitmap(Bitmap bitmap, float left, float top, Paint paint) {
    image(bitmap, null, new RectF(left, top, left + bitmap.getWidth(), top + bitmap.getHeight()), paint);
  }
  public void drawBitmap(Bitmap bitmap, Rect src, Rect dst, Paint paint) {
    image(bitmap, src, new RectF(dst.left, dst.top, dst.right, dst.bottom), paint);
  }
  public void drawBitmap(Bitmap bitmap, Rect src, RectF dst, Paint paint) {
    image(bitmap, src, dst, paint);
  }
  public void drawBitmap(Bitmap bitmap, Matrix m, Paint paint) {
    Graphics2D g = graphics(paint);
    if (g == null) return;
    try {
      g.drawImage(bitmap.image, m.transform(), null);
    } finally {
      g.dispose();
    }
  }
  public void drawText(String text, float x, float y, Paint paint) {
    Graphics2D g = graphics(paint);
    if (g == null || text.isEmpty()) {
      if (g != null) g.dispose();
      return;
    }
    try {
      g.setFont(paint.font());
      float advance = paint.measureText(text);
      float origin = switch (paint.getTextAlign()) {
        case CENTER -> x - advance / 2;
        case RIGHT -> x - advance;
        default -> x;
      };
      var glyphs = new java.awt.font.TextLayout(text, paint.font(), g.getFontRenderContext());
      Shape outline = glyphs.getOutline(AffineTransform.getTranslateInstance(origin, y));
      if (paint.getStyle() == Paint.Style.FILL) {
        glyphs.draw(g, origin, y);
      } else {
        if (paint.getStyle() == Paint.Style.FILL_AND_STROKE) g.fill(outline);
        g.setStroke(stroke(paint));
        g.draw(outline);
      }
    } finally {
      g.dispose();
    }
  }
  public void drawText(CharSequence text, int start, int end, float x, float y, Paint paint) {
    drawText(text.subSequence(start, end).toString(), x, y, paint);
  }
  public void drawText(char[] text, int index, int count, float x, float y, Paint paint) {
    drawText(new String(text, index, count), x, y, paint);
  }
}
