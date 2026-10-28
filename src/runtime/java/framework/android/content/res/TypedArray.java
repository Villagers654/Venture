package android.content.res;
import android.util.TypedValue;
import com.venture.runtime.BinaryResources.Value;
public class TypedArray {
  private final Resources resources;
  private Value[] values;
  TypedArray(Resources r, Value[] v) {
    resources = r;
    values = v;
  }
  public int length() {
    return values.length;
  }
  public int getIndexCount() {
    int n = 0;
    for (Value v : values)
      if (v != null)
        n++;
    return n;
  }
  public int getIndex(int index) {
    for (int i = 0; i < values.length; i++)
      if (values[i] != null && index-- == 0)
        return i;
    throw new IndexOutOfBoundsException();
  }
  private Value value(int i) {
    Value v = values[i];
    if (v != null && v.type() == 1 && v.data() == 0)
      return null;
    return v != null && v.type() == 1 ? resources.value(v.data()) : v;
  }
  public boolean hasValue(int i) {
    return values[i] != null && values[i].type() != 0;
  }
  public int getResourceId(int i, int d) {
    Value v = values[i];
    if (v != null && v.type() == 1 && v.data() == 0)
      return d;
    return v != null && v.type() == 1 ? v.data() : d;
  }
  public String getString(int i) {
    Value v = value(i);
    return v == null ? null : v.encoded();
  }
  public CharSequence getText(int i) {
    return getString(i);
  }
  public int getInt(int i, int d) {
    Value v = value(i);
    return v == null ? d : v.data();
  }
  public int getInteger(int i, int d) {
    return getInt(i, d);
  }
  public int getColor(int i, int d) {
    return getInt(i, d);
  }
  public boolean getBoolean(int i, boolean d) {
    Value v = value(i);
    return v == null ? d : v.data() != 0;
  }
  public float getFloat(int i, float d) {
    Value v = value(i);
    return v == null ? d : v.type() == 4 ? Float.intBitsToFloat(v.data()) : v.data();
  }
  public float getDimension(int i, float d) {
    Value v = value(i);
    return v == null ? d : TypedValue.complexToDimension(v.data(), resources.getDisplayMetrics());
  }
  public int getDimensionPixelSize(int i, int d) {
    return Math.round(getDimension(i, d));
  }
  public int getDimensionPixelOffset(int i, int d) {
    return (int) getDimension(i, d);
  }
  public int getLayoutDimension(int i, int d) {
    Value v = value(i);
    return v != null && v.type() == 5 ? getDimensionPixelSize(i, d) : getInt(i, d);
  }
  public android.graphics.drawable.Drawable getDrawable(int i) {
    Value v = value(i);
    if (v == null)
      return null;
    if (v.type() >= 28 && v.type() <= 31)
      return new android.graphics.drawable.ColorDrawable(v.data());
    int id = getResourceId(i, 0);
    return id == 0 ? null : resources.getDrawable(id);
  }
  public boolean getValue(int i, TypedValue out) {
    Value v = value(i);
    if (v == null)
      return false;
    out.type = v.type();
    out.data = v.data();
    out.string = v.text();
    out.resourceId = getResourceId(i, 0);
    return true;
  }
  public TypedValue peekValue(int i) {
    TypedValue v = new TypedValue();
    return getValue(i, v) ? v : null;
  }
  public int getType(int i) {
    Value v = value(i);
    return v == null ? 0 : v.type();
  }
  public void recycle() {
    values = null;
  }
}
