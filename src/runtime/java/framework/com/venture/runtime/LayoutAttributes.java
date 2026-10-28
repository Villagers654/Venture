package com.venture.runtime;
import android.util.AttributeSet;
public final class LayoutAttributes implements AttributeSet {
  public final BinaryResources.Node node;
  public LayoutAttributes(BinaryResources.Node n) {
    node = n;
  }
  private BinaryResources.Value value(int i) {
    return node.orderedAttributes.get(i).value();
  }
  private int index(String ns, String name) {
    for (int i = 0; i < getAttributeCount(); i++) {
      var a = node.orderedAttributes.get(i);
      if (a.name().equals(name) && (ns == null || a.namespace().equals(ns)))
        return i;
    }
    return -1;
  }
  public int getAttributeCount() {
    return node.orderedAttributes.size();
  }
  public String getAttributeName(int i) {
    return node.orderedAttributes.get(i).name();
  }
  public String getAttributeValue(int i) {
    return value(i).encoded();
  }
  public String getAttributeValue(String ns, String name) {
    int i = index(ns, name);
    return i < 0 ? null : getAttributeValue(i);
  }
  public String getPositionDescription() {
    return "binary XML <" + node.name + ">";
  }
  public int getAttributeNameResource(int i) {
    return node.orderedAttributes.get(i).resourceId();
  }
  public int getAttributeListValue(int i, String[] options, int def) {
    if (i < 0)
      return def;
    String v = getAttributeValue(i);
    for (int n = 0; n < options.length; n++)
      if (options[n].equals(v))
        return n;
    return def;
  }
  public boolean getAttributeBooleanValue(int i, boolean def) {
    return i < 0                ? def
        : value(i).type() == 18 ? value(i).data() != 0
                                : Boolean.parseBoolean(getAttributeValue(i));
  }
  public int getAttributeResourceValue(int i, int def) {
    return i < 0 || value(i).type() != 1 ? def : value(i).data();
  }
  public int getAttributeIntValue(int i, int def) {
    return i < 0 ? def : value(i).data();
  }
  public int getAttributeUnsignedIntValue(int i, int def) {
    int v = getAttributeIntValue(i, def);
    return v < 0 ? def : v;
  }
  public float getAttributeFloatValue(int i, float def) {
    return i < 0               ? def
        : value(i).type() == 4 ? Float.intBitsToFloat(value(i).data())
                               : value(i).data();
  }
  public int getAttributeListValue(String ns, String name, String[] options, int def) {
    return getAttributeListValue(index(ns, name), options, def);
  }
  public boolean getAttributeBooleanValue(String ns, String name, boolean def) {
    return getAttributeBooleanValue(index(ns, name), def);
  }
  public int getAttributeResourceValue(String ns, String name, int def) {
    return getAttributeResourceValue(index(ns, name), def);
  }
  public int getAttributeIntValue(String ns, String name, int def) {
    return getAttributeIntValue(index(ns, name), def);
  }
  public int getAttributeUnsignedIntValue(String ns, String name, int def) {
    return getAttributeUnsignedIntValue(index(ns, name), def);
  }
  public float getAttributeFloatValue(String ns, String name, float def) {
    return getAttributeFloatValue(index(ns, name), def);
  }
  public String getIdAttribute() {
    return getAttributeValue(null, "id");
  }
  public String getClassAttribute() {
    return getAttributeValue(null, "class");
  }
  public int getIdAttributeResourceValue(int def) {
    return getAttributeResourceValue(null, "id", def);
  }
  public int getStyleAttribute() {
    return getAttributeResourceValue(null, "style", 0);
  }
  public BinaryResources.Value byId(int id) {
    return node.orderedAttributes.stream()
        .filter(a -> a.resourceId() == id)
        .map(BinaryResources.Attribute::value)
        .findFirst()
        .orElse(null);
  }
}
