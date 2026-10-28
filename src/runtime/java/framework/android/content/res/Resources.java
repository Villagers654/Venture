package android.content.res;
import android.util.*;
import com.venture.runtime.*;
import java.io.*;
import java.util.*;

public class Resources {
  public static class NotFoundException extends RuntimeException {
    public NotFoundException() {}
    public NotFoundException(String m) {
      super(m);
    }
  }
  private final AssetManager assets;
  private final DisplayMetrics metrics;
  private final Configuration configuration;
  private final ResourceTable table;
  public Resources(AssetManager a, DisplayMetrics m, Configuration c) {
    assets = a;
    metrics = m;
    configuration = c;
    table = new ResourceTable(RuntimeEnvironment.host().readAsset("resources.arsc"));
  }
  public static Resources getSystem() {
    return RuntimeEnvironment.resources();
  }
  public AssetManager getAssets() {
    return assets;
  }
  public DisplayMetrics getDisplayMetrics() {
    return metrics;
  }
  public Configuration getConfiguration() {
    return configuration;
  }
  public void updateConfiguration(Configuration c, DisplayMetrics d) {
    if (c != null)
      configuration.setTo(c);
    if (d != null)
      metrics.setTo(d);
  }
  private ResourceTable tableFor(int id) {
    return (id >>> 24) == 1 ? SystemAssets.table() : table;
  }
  private record Resolved(int id, BinaryResources.Value value) {}
  private Resolved scalar(int id) {
    Set<Integer> seen = new HashSet<>();
    for (;;) {
      if (!seen.add(id))
        throw new NotFoundException("Cyclic resource reference");
      BinaryResources.Value value = tableFor(id).entry(id, configuration, metrics).value();
      if (value == null)
        throw new NotFoundException("Bag requested as scalar");
      if (value.type() != 1)
        return new Resolved(id, value);
      if (value.data() == 0)
        return new Resolved(id, new BinaryResources.Value(0, 0, null));
      id = value.data(); // References may cross application / Android package tables.
    }
  }
  BinaryResources.Value value(int id) {
    return scalar(id).value;
  }
  public String getString(int id) {
    return getText(id).toString();
  }
  public String getString(int id, Object... args) {
    return String.format(configuration.locale, getString(id), args);
  }
  public CharSequence getText(int id) {
    BinaryResources.Value v = value(id);
    if (v.type() != 3)
      throw new NotFoundException("Not a string");
    return v.text();
  }
  public CharSequence getText(int id, CharSequence fallback) {
    try {
      return getText(id);
    } catch (NotFoundException e) {
      return fallback;
    }
  }
  public boolean getBoolean(int id) {
    return value(id).data() != 0;
  }
  public int getInteger(int id) {
    return value(id).data();
  }
  public int getColor(int id) {
    return value(id).data();
  }
  public int getColor(int id, Theme theme) {
    return getColor(id);
  }
  public float getDimension(int id) {
    return TypedValue.complexToDimension(value(id).data(), metrics);
  }
  public int getDimensionPixelSize(int id) {
    return Math.round(getDimension(id));
  }
  public int getDimensionPixelOffset(int id) {
    return (int) getDimension(id);
  }
  public float getFraction(int id, int base, int parent) {
    TypedValue v = new TypedValue();
    getValue(id, v, true);
    return v.getFraction(base, parent);
  }
  public int getIdentifier(String name, String type, String pkg) {
    return (name.startsWith("android:") || "android".equals(pkg) ? SystemAssets.table() : table)
        .identifier(name, type, pkg);
  }
  public String getResourcePackageName(int id) {
    return tableFor(id).entry(id, configuration, metrics).packageName();
  }
  public String getResourceTypeName(int id) {
    return tableFor(id).entry(id, configuration, metrics).type();
  }
  public String getResourceEntryName(int id) {
    return tableFor(id).entry(id, configuration, metrics).name();
  }
  public String getResourceName(int id) {
    return getResourcePackageName(id) + ":" + getResourceTypeName(id) + "/"
        + getResourceEntryName(id);
  }
  public void getValue(int id, TypedValue out, boolean resolve) {
    Resolved result = resolve
        ? scalar(id)
        : new Resolved(id, tableFor(id).entry(id, configuration, metrics).value());
    BinaryResources.Value v = result.value;
    if (v == null)
      throw new NotFoundException("Bag requested as scalar");
    out.type = v.type();
    out.data = v.data();
    out.string = v.text();
    out.resourceId = result.id;
  }
  public InputStream openRawResource(int id) {
    try {
      Resolved result = scalar(id);
      if (result.value.type() != 3)
        throw new NotFoundException("Not a resource file");
      return (result.id >>> 24) == 1
          ? new ByteArrayInputStream(SystemAssets.read(result.value.text()))
          : assets.openNonAsset(result.value.text());
    } catch (IOException e) {
      throw new NotFoundException(e.toString());
    }
  }
  public String[] getStringArray(int id) {
    return bag(id, new HashSet<>())
        .values()
        .stream()
        .map(v -> v.type() == 1 ? getString(v.data()) : v.text())
        .toArray(String[] ::new);
  }
  public CharSequence[] getTextArray(int id) {
    return getStringArray(id);
  }
  public int[] getIntArray(int id) {
    return bag(id, new HashSet<>())
        .values()
        .stream()
        .mapToInt(v -> v.type() == 1 ? getInteger(v.data()) : v.data())
        .toArray();
  }
  Map<Integer, BinaryResources.Value> bag(int id, Set<Integer> seen) {
    if (!seen.add(id))
      throw new NotFoundException("Cyclic style parent");
    ResourceTable.Entry e = tableFor(id).entry(id, configuration, metrics);
    Map<Integer, BinaryResources.Value> out = new TreeMap<>();
    if (e.parent() != 0)
      out.putAll(bag(e.parent(), seen));
    out.putAll(e.bag());
    return out;
  }
  public android.graphics.drawable.Drawable getDrawable(int id) {
    return getDrawable(id, null);
  }
  public android.graphics.drawable.Drawable getDrawable(int id, Theme theme) {
    BinaryResources.Value v = value(id);
    if (v.type() >= 28 && v.type() <= 31)
      return new android.graphics.drawable.ColorDrawable(v.data());
    if (v.type() != 3)
      throw new NotFoundException("Not a drawable");
    try (var in = openRawResource(id)) {
      if (v.text().endsWith(".xml")) {
        var node = BinaryResources.xml(in.readAllBytes());
        return node.name.equals("vector") ? new android.graphics.drawable.VectorDrawable(node)
                                          : new android.graphics.drawable.XmlDrawable(node);
      }
      var bitmap = android.graphics.BitmapFactory.decodeStream(in);
      if (bitmap == null)
        throw new NotFoundException("Unsupported image " + v.text());
      return new android.graphics.drawable.BitmapDrawable(this, bitmap);
    } catch (IOException e) {
      throw new NotFoundException(e.toString());
    }
  }
  public XmlResourceParser getXml(int id) {
    try (var input = openRawResource(id)) {
      return new ResourceXmlParser(input.readAllBytes());
    } catch (IOException e) {
      throw new NotFoundException(e.toString());
    }
  }
  public XmlResourceParser getLayout(int id) {
    return getXml(id);
  }
  public XmlResourceParser getAnimation(int id) {
    return getXml(id);
  }
  public Theme newTheme() {
    return new Theme();
  }
  public TypedArray obtainTypedArray(int id) {
    return new TypedArray(
        this, bag(id, new HashSet<>()).values().toArray(BinaryResources.Value[] ::new));
  }
  public final class Theme {
    private final Map<Integer, BinaryResources.Value> attributes = new HashMap<>();
    private record Style(int id, boolean force) {}
    private final List<Style> styles = new ArrayList<>();
    public void rebase() {
      attributes.clear();
      for (Style s : styles)
        for (var e : bag(s.id, new HashSet<>()).entrySet())
          if (s.force || !attributes.containsKey(e.getKey()))
            attributes.put(e.getKey(), e.getValue());
    }
    public void applyStyle(int id, boolean force) {
      if (id == 0)
        return;
      styles.add(new Style(id, force));
      for (var e : bag(id, new HashSet<>()).entrySet())
        if (force || !attributes.containsKey(e.getKey()))
          attributes.put(e.getKey(), e.getValue());
    }
    public void setTo(Theme other) {
      attributes.clear();
      attributes.putAll(other.attributes);
      styles.clear();
      styles.addAll(other.styles);
    }
    public Resources getResources() {
      return Resources.this;
    }
    public boolean resolveAttribute(int id, TypedValue out, boolean refs) {
      BinaryResources.Value v = attributes.get(id);
      if (v == null)
        return false;
      Set<Integer> seen = new HashSet<>();
      while (v.type() == 2) {
        if (!seen.add(v.data()))
          throw new NotFoundException("Cyclic attribute");
        v = attributes.get(v.data());
        if (v == null)
          return false;
      }
      if (refs && v.type() == 1) {
        getValue(v.data(), out, true);
        return true;
      }
      out.type = v.type();
      out.data = v.data();
      out.string = v.text();
      out.resourceId = v.type() == 1 ? v.data() : 0;
      return true;
    }
    public TypedArray obtainStyledAttributes(int[] ids) {
      return obtainStyledAttributes(0, ids);
    }
    public TypedArray obtainStyledAttributes(int style, int[] ids) {
      Map<Integer, BinaryResources.Value> map = new HashMap<>(attributes);
      if (style != 0)
        map.putAll(bag(style, new HashSet<>()));
      BinaryResources.Value[] values = new BinaryResources.Value[ids.length];
      for (int i = 0; i < ids.length; i++) values[i] = map.get(ids[i]);
      return new TypedArray(Resources.this, values);
    }
    public TypedArray obtainStyledAttributes(AttributeSet set, int[] ids, int attr, int def) {
      TypedValue v = new TypedValue();
      int style = attr != 0 && resolveAttribute(attr, v, false) ? v.resourceId : def;
      Map<Integer, BinaryResources.Value> map = new HashMap<>(attributes);
      if (style != 0)
        map.putAll(bag(style, new HashSet<>()));
      if (set != null && set.getStyleAttribute() != 0)
        map.putAll(bag(set.getStyleAttribute(), new HashSet<>()));
      BinaryResources.Value[] values = new BinaryResources.Value[ids.length];
      for (int i = 0; i < ids.length; i++) {
        BinaryResources.Value value = map.get(ids[i]);
        if (set instanceof LayoutAttributes a && a.byId(ids[i]) != null)
          value = a.byId(ids[i]);
        if (value != null && value.type() == 2) {
          TypedValue resolved = new TypedValue();
          if (resolveAttribute(value.data(), resolved, false))
            value = new BinaryResources.Value(resolved.type, resolved.data,
                resolved.string == null ? null : resolved.string.toString());
        }
        values[i] = value;
      }
      return new TypedArray(Resources.this, values);
    }
  }
}
