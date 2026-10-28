package android.view;
import java.util.ArrayList;
import java.util.HashMap;

/** Android pointer-event values. Host input must explicitly construct an event. */
public class MotionEvent extends InputEvent {
  public static final int ACTION_DOWN = 0, ACTION_UP = 1, ACTION_MOVE = 2, ACTION_CANCEL = 3,
      ACTION_OUTSIDE = 4, ACTION_POINTER_DOWN = 5, ACTION_POINTER_UP = 6, ACTION_HOVER_MOVE = 7,
      ACTION_SCROLL = 8, ACTION_MASK = 255, ACTION_POINTER_INDEX_MASK = 0xff00,
      ACTION_POINTER_INDEX_SHIFT = 8, TOOL_TYPE_UNKNOWN = 0, TOOL_TYPE_FINGER = 1,
      TOOL_TYPE_STYLUS = 2, TOOL_TYPE_MOUSE = 3, AXIS_X = 0, AXIS_Y = 1, AXIS_PRESSURE = 2,
      AXIS_SIZE = 3, AXIS_TOUCH_MAJOR = 4, AXIS_TOUCH_MINOR = 5, AXIS_TOOL_MAJOR = 6,
      AXIS_TOOL_MINOR = 7, AXIS_ORIENTATION = 8, CLASSIFICATION_NONE = 0;
  public static final class PointerProperties {
    public int id, toolType;
    public void clear() { id = -1; toolType = TOOL_TYPE_UNKNOWN; }
    public void copyFrom(PointerProperties other) { id = other.id; toolType = other.toolType; }
  }
  public static final class PointerCoords {
    public float x, y, pressure, size, touchMajor, touchMinor, toolMajor, toolMinor, orientation;
    private final HashMap<Integer, Float> axes = new HashMap<>();
    public void clear() {
      x = y = pressure = size = touchMajor = touchMinor = toolMajor = toolMinor = orientation = 0; axes.clear();
    }
    public void copyFrom(PointerCoords p) {
      x = p.x; y = p.y; pressure = p.pressure; size = p.size; touchMajor = p.touchMajor;
      touchMinor = p.touchMinor; toolMajor = p.toolMajor; toolMinor = p.toolMinor; orientation = p.orientation;
      axes.clear(); axes.putAll(p.axes);
    }
    public float getAxisValue(int axis) {
      return switch (axis) {
        case 0 -> x; case 1 -> y; case 2 -> pressure; case 3 -> size; case 4 -> touchMajor;
        case 5 -> touchMinor; case 6 -> toolMajor; case 7 -> toolMinor; case 8 -> orientation;
        default -> axes.getOrDefault(axis, 0f);
      };
    }
    public void setAxisValue(int axis, float value) {
      switch (axis) {
        case 0 -> x = value; case 1 -> y = value; case 2 -> pressure = value; case 3 -> size = value;
        case 4 -> touchMajor = value; case 5 -> touchMinor = value; case 6 -> toolMajor = value;
        case 7 -> toolMinor = value; case 8 -> orientation = value; default -> axes.put(axis, value);
      }
    }
  }
  private record Sample(long time, PointerCoords[] points) {}
  private long downTime;
  private int action, actionButton, meta, buttons, device = -1, edges, source, flags;
  private float xPrecision = 1, yPrecision = 1;
  private PointerProperties[] properties = {};
  private Sample sample = new Sample(0, new PointerCoords[0]);
  private final ArrayList<Sample> history = new ArrayList<>();
  private static PointerCoords[] copy(PointerCoords[] values, int count) {
    if (values.length < count) throw new IllegalArgumentException("Missing pointer coordinates");
    PointerCoords[] result = new PointerCoords[count];
    for (int i = 0; i < count; ++i) { result[i] = new PointerCoords(); result[i].copyFrom(values[i]); }
    return result;
  }
  public static MotionEvent obtain(long down, long time, int action, float x, float y, int meta) {
    return obtain(down, time, action, x, y, 1, 1, meta, 1, 1, 0, 0);
  }
  public static MotionEvent obtain(long down, long time, int action, float x, float y, float pressure,
      float size, int meta, float xp, float yp, int device, int edges) {
    PointerProperties property = new PointerProperties(); property.toolType = TOOL_TYPE_FINGER;
    PointerCoords coords = new PointerCoords(); coords.x = x; coords.y = y; coords.pressure = pressure; coords.size = size;
    return obtain(down, time, action, 1, new PointerProperties[]{property}, new PointerCoords[]{coords},
        meta, 0, xp, yp, device, edges, 0x1002, 0);
  }
  public static MotionEvent obtain(long down, long time, int action, int count, PointerProperties[] properties,
      PointerCoords[] coords, int meta, int buttons, float xp, float yp, int device, int edges, int source, int flags) {
    if (count < 1 || count > 32 || properties.length < count) throw new IllegalArgumentException("Pointer count");
    MotionEvent event = new MotionEvent(); event.downTime = down; event.action = action; event.meta = meta;
    event.buttons = buttons; event.device = device; event.edges = edges; event.source = source; event.flags = flags;
    event.xPrecision = xp; event.yPrecision = yp; event.properties = new PointerProperties[count];
    for (int i = 0; i < count; ++i) {
      event.properties[i] = new PointerProperties(); event.properties[i].copyFrom(properties[i]);
    }
    event.sample = new Sample(time, copy(coords, count)); return event;
  }
  public int getSource() { return source; }
  public void setSource(int value) { source = value; }
  public int getDeviceId() { return device; }
  public long getEventTime() { return sample.time(); }
  public long getDownTime() { return downTime; }
  public int getAction() { return action; }
  public int getActionMasked() { return action & ACTION_MASK; }
  public int getActionIndex() { return (action & ACTION_POINTER_INDEX_MASK) >> 8; }
  public void setAction(int value) { action = value; }
  public int getMetaState() { return meta; }
  public int getButtonState() { return buttons; }
  public int getActionButton() { return actionButton; }
  public void setActionButton(int value) { actionButton = value; }
  public int getFlags() { return flags; }
  public int getEdgeFlags() { return edges; }
  public int getClassification() { return CLASSIFICATION_NONE; }
  public float getXPrecision() { return xPrecision; }
  public float getYPrecision() { return yPrecision; }
  public int getPointerCount() { return properties.length; }
  public int getPointerId(int index) { return properties[index].id; }
  public int getToolType(int index) { return properties[index].toolType; }
  public int findPointerIndex(int id) {
    for (int i = 0; i < properties.length; ++i) if (properties[i].id == id) return i;
    return -1;
  }
  public float getAxisValue(int axis) { return getAxisValue(axis, 0); }
  public float getAxisValue(int axis, int index) { return sample.points()[index].getAxisValue(axis); }
  public float getX() { return getX(0); }
  public float getY() { return getY(0); }
  public float getX(int index) { return getAxisValue(AXIS_X, index); }
  public float getY(int index) { return getAxisValue(AXIS_Y, index); }
  public float getRawX() { return getX(); }
  public float getRawY() { return getY(); }
  public float getRawX(int index) { return getX(index); }
  public float getRawY(int index) { return getY(index); }
  public float getPressure() { return getAxisValue(AXIS_PRESSURE); }
  public float getPressure(int index) { return getAxisValue(AXIS_PRESSURE, index); }
  public float getSize() { return getAxisValue(AXIS_SIZE); }
  public float getSize(int index) { return getAxisValue(AXIS_SIZE, index); }
  public int getHistorySize() { return history.size(); }
  public long getHistoricalEventTime(int position) { return history.get(position).time(); }
  public float getHistoricalAxisValue(int axis, int index, int position) {
    return history.get(position).points()[index].getAxisValue(axis);
  }
  public float getHistoricalAxisValue(int axis, int position) { return getHistoricalAxisValue(axis, 0, position); }
  public void addBatch(long time, PointerCoords[] coords, int meta) {
    if (getActionMasked() != ACTION_MOVE && getActionMasked() != ACTION_HOVER_MOVE)
      throw new IllegalStateException("Only motion events can batch samples");
    PointerCoords[] values = copy(coords, properties.length);
    history.add(sample); sample = new Sample(time, values); this.meta |= meta;
  }
  public void recycle() { history.clear(); }
}
