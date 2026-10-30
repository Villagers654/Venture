package android.util;
public class TypedValue {
  public static final int TYPE_NULL = 0, TYPE_REFERENCE = 1, TYPE_ATTRIBUTE = 2, TYPE_STRING = 3,
                          TYPE_FLOAT = 4, TYPE_DIMENSION = 5, TYPE_FRACTION = 6, TYPE_INT_DEC = 16,
                          TYPE_INT_HEX = 17, TYPE_INT_BOOLEAN = 18, TYPE_FIRST_COLOR_INT = 28,
                          TYPE_LAST_COLOR_INT = 31;
  public int type, data, resourceId, assetCookie, density, changingConfigurations;
  public CharSequence string;
  public float getFloat() {
    return Float.intBitsToFloat(data);
  }
  public static float complexToFloat(int complex) {
    float[] radix = {1f / 256, 1f / 32768, 1f / 8388608, 1f / 2147483648f};
    return (complex & 0xffffff00) * radix[(complex >> 4) & 3];
  }
  public static float applyDimension(int unit, float value, DisplayMetrics m) {
    return switch (unit) {
      case 0 -> value;
      case 1 -> value* m.density;
      case 2 -> value* m.scaledDensity;
      case 3 -> value* m.xdpi / 72;
      case 4 -> value* m.xdpi;
      case 5 -> value* m.xdpi / 25.4f;
      default -> 0;
    };
  }
  public static float complexToDimension(int v, DisplayMetrics m) {
    return applyDimension(v & 15, complexToFloat(v), m);
  }
  // AOSP: rounds half up, and a nonzero size is at least one pixel.
  public static int complexToDimensionPixelSize(int data, DisplayMetrics metrics) {
    float value = complexToFloat(data);
    float f = applyDimension(data & 15, value, metrics);
    int res = (int) (f >= 0 ? f + 0.5f : f - 0.5f);
    if (res != 0) return res;
    if (value == 0) return 0;
    return value > 0 ? 1 : -1;
  }
  public static int complexToDimensionPixelOffset(int data, DisplayMetrics metrics) {
    return (int) applyDimension(data & 15, complexToFloat(data), metrics);
  }
  public int getComplexUnit() { return data & 15; }
  public float getDimension(DisplayMetrics m) {
    return complexToDimension(data, m);
  }
  public float getFraction(float base, float parent) {
    return complexToFloat(data) * ((data & 15) == 1 ? parent : base);
  }
  public CharSequence coerceToString() {
    return type == 3 ? string : String.valueOf(data);
  }
}
