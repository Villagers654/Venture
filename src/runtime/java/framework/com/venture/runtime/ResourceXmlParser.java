package com.venture.runtime;

import android.content.res.XmlResourceParser;
import java.util.*;

/** KXml token stream over compiled XML with its original resource attribute IDs. */
public final class ResourceXmlParser extends org.kxml2.io.KXmlParser implements XmlResourceParser {
  private final Map<String, Integer> ids = new HashMap<>();
  public ResourceXmlParser(byte[] bytes) {
    try {
      setFeature(FEATURE_PROCESS_NAMESPACES, true);
      if (bytes.length >= 2 && bytes[0] == 3 && bytes[1] == 0) {
        var root = BinaryResources.xml(bytes);
        StringBuilder xml = new StringBuilder();
        Map<String, String> namespaces = new LinkedHashMap<>();
        collect(root, namespaces);
        append(root, xml, namespaces, true);
        setInput(new java.io.StringReader(xml.toString()));
      } else
        setInput(new java.io.ByteArrayInputStream(bytes), "UTF-8");
    } catch (org.xmlpull.v1.XmlPullParserException e) {
      throw new IllegalArgumentException(e);
    }
  }
  private void collect(BinaryResources.Node n, Map<String, String> namespaces) {
    for (var a : n.orderedAttributes) {
      if (!a.namespace().isEmpty())
        namespaces.computeIfAbsent(a.namespace(), k -> "ns" + namespaces.size());
      ids.put(a.namespace() + ":" + a.name(), a.resourceId());
    }
    for (var c : n.children) collect(c, namespaces);
  }
  private static String escape(String value) {
    return value.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace("\"", "&quot;")
        .replace("\r", "&#13;")
        .replace("\n", "&#10;");
  }
  private static void append(
      BinaryResources.Node n, StringBuilder xml, Map<String, String> namespaces, boolean root) {
    xml.append('<').append(n.name);
    if (root)
      for (var e : namespaces.entrySet())
        xml.append(" xmlns:")
            .append(e.getValue())
            .append("=\"")
            .append(escape(e.getKey()))
            .append('"');
    for (var a : n.orderedAttributes) {
      xml.append(' ');
      if (!a.namespace().isEmpty())
        xml.append(namespaces.get(a.namespace())).append(':');
      xml.append(a.name()).append("=\"").append(escape(a.value().encoded())).append('"');
    }
    xml.append('>').append(escape(n.text));
    for (var child : n.children) append(child, xml, namespaces, false);
    xml.append("</").append(n.name).append('>');
  }
  public void close() {
    try {
      setInput((java.io.Reader) null);
    } catch (org.xmlpull.v1.XmlPullParserException e) {
      throw new IllegalStateException(e);
    }
  }
  public int getAttributeNameResource(int i) {
    return ids.getOrDefault(getAttributeNamespace(i) + ":" + getAttributeName(i), 0);
  }
  private static int integer(String s, int def) {
    if (s == null)
      return def;
    try {
      if (s.startsWith("@") || s.startsWith("?"))
        s = s.substring(1);
      return (int) Long.decode(s).longValue();
    } catch (NumberFormatException e) {
      return def;
    }
  }
  private static int option(String value, String[] options, int def) {
    for (int i = 0; i < options.length; i++)
      if (options[i].equals(value))
        return i;
    return def;
  }
  public int getAttributeListValue(String ns, String name, String[] options, int def) {
    return option(getAttributeValue(ns, name), options, def);
  }
  public boolean getAttributeBooleanValue(String ns, String name, boolean def) {
    String s = getAttributeValue(ns, name);
    return s == null ? def : Boolean.parseBoolean(s);
  }
  public int getAttributeResourceValue(String ns, String name, int def) {
    String s = getAttributeValue(ns, name);
    return s != null && s.startsWith("@") ? integer(s, def) : def;
  }
  public int getAttributeIntValue(String ns, String name, int def) {
    return integer(getAttributeValue(ns, name), def);
  }
  public int getAttributeUnsignedIntValue(String ns, String name, int def) {
    int v = getAttributeIntValue(ns, name, def);
    return v < 0 ? def : v;
  }
  public float getAttributeFloatValue(String ns, String name, float def) {
    try {
      return Float.parseFloat(getAttributeValue(ns, name));
    } catch (NullPointerException | NumberFormatException e) {
      return def;
    }
  }
  public int getAttributeListValue(int i, String[] options, int def) {
    return option(getAttributeValue(i), options, def);
  }
  public boolean getAttributeBooleanValue(int i, boolean def) {
    String s = getAttributeValue(i);
    return s == null ? def : Boolean.parseBoolean(s);
  }
  public int getAttributeResourceValue(int i, int def) {
    String s = getAttributeValue(i);
    return s != null && s.startsWith("@") ? integer(s, def) : def;
  }
  public int getAttributeIntValue(int i, int def) {
    return integer(getAttributeValue(i), def);
  }
  public int getAttributeUnsignedIntValue(int i, int def) {
    int v = getAttributeIntValue(i, def);
    return v < 0 ? def : v;
  }
  public float getAttributeFloatValue(int i, float def) {
    try {
      return Float.parseFloat(getAttributeValue(i));
    } catch (NullPointerException | NumberFormatException e) {
      return def;
    }
  }
  public String getIdAttribute() {
    return getAttributeValue("http://schemas.android.com/apk/res/android", "id");
  }
  public String getClassAttribute() {
    return getAttributeValue(null, "class");
  }
  public int getIdAttributeResourceValue(int def) {
    return integer(getIdAttribute(), def);
  }
  public int getStyleAttribute() {
    return getAttributeResourceValue(null, "style", 0);
  }
}
