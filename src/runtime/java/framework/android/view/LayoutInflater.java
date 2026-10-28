package android.view;
import android.content.Context;
import android.util.AttributeSet;
public class LayoutInflater {
  private final Context context;
  private Factory factory;
  private Factory2 factory2;
  public interface Factory {
    View onCreateView(String name, Context context, AttributeSet attrs);
  }
  public interface Factory2 extends Factory {
    View onCreateView(View parent, String name, Context context, AttributeSet attrs);
  }
  public LayoutInflater(Context c) {
    context = c;
  }
  public static LayoutInflater from(Context c) {
    return (LayoutInflater) c.getSystemService("layout_inflater");
  }
  public LayoutInflater cloneInContext(Context c) {
    LayoutInflater i = new LayoutInflater(c);
    i.factory = factory;
    i.factory2 = factory2;
    return i;
  }
  public Context getContext() {
    return context;
  }
  public void setFactory(Factory f) {
    factory = f;
  }
  public Factory getFactory() {
    return factory;
  }
  public void setFactory2(Factory2 f) {
    factory2 = f;
    factory = f;
  }
  public Factory2 getFactory2() {
    return factory2;
  }
  public View inflate(int resource, ViewGroup root) {
    return inflate(resource, root, root != null);
  }
  public View inflate(int resource, ViewGroup root, boolean attach) {
    try (var input = context.getResources().openRawResource(resource)) {
      var node = com.venture.runtime.BinaryResources.xml(input.readAllBytes());
      if (node.name.equals("merge")) {
        if (root == null || !attach)
          throw new IllegalArgumentException("merge requires attached root");
        for (var child : node.children) inflateNode(child, root, true);
        return root;
      }
      View result = inflateNode(node, root, attach);
      return attach ? root : result;
    } catch (java.io.IOException e) {
      throw new InflateException(e.toString());
    }
  }
  private View inflateNode(
      com.venture.runtime.BinaryResources.Node node, ViewGroup parent, boolean attach) {
    var attrs = new com.venture.runtime.LayoutAttributes(node);
    if (node.name.equals("include")) {
      int layout = attrs.getAttributeResourceValue(null, "layout", 0);
      return inflate(layout, parent, true);
    }
    String name = node.name.equals("view") ? attrs.getClassAttribute() : node.name;
    View view = factory2 != null ? factory2.onCreateView(parent, name, context, attrs)
        : factory != null        ? factory.onCreateView(name, context, attrs)
                                 : null;
    if (view == null)
      try {
        String type = name.contains(".")                        ? name
            : name.equals("View") || name.equals("SurfaceView") ? "android.view." + name
                                                                : "android.widget." + name;
        Class<?> cls = context.getClassLoader().loadClass(type);
        view = (View) cls.getConstructor(Context.class, AttributeSet.class)
                   .newInstance(context, attrs);
      } catch (ReflectiveOperationException e) {
        throw new InflateException("Cannot inflate " + name, e);
      }
    view.setId(attrs.getIdAttributeResourceValue(View.NO_ID));
    int w = dimension(node, "layout_width", -2), h = dimension(node, "layout_height", -2);
    ViewGroup.LayoutParams params = parent instanceof android.widget.FrameLayout
        ? new android.widget.FrameLayout.LayoutParams(w, h)
        : parent instanceof android.widget.LinearLayout
        ? new android.widget.LinearLayout.LayoutParams(w, h)
        : new ViewGroup.LayoutParams(w, h);
    view.setLayoutParams(params);
    if (view instanceof ViewGroup group)
      for (var child : node.children) inflateNode(child, group, true);
    if (attach && parent != null)
      parent.addView(view, params);
    view.onFinishInflate();
    return view;
  }
  private int dimension(
      com.venture.runtime.BinaryResources.Node node, String name, int fallback) {
    var value = node.attributes.get(name);
    if (value == null)
      return fallback;
    if (value.type() == 1)
      return context.getResources().getDimensionPixelSize(value.data());
    if (value.type() == 5)
      return Math.round(android.util.TypedValue.complexToDimension(
          value.data(), context.getResources().getDisplayMetrics()));
    return value.data();
  }
}
