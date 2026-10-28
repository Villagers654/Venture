package android.util;
public interface AttributeSet {
  int getAttributeCount();
  String getAttributeName(int index);
  String getAttributeValue(int index);
  String getAttributeValue(String namespace, String name);
  String getPositionDescription();
  int getAttributeNameResource(int index);
  int getAttributeListValue(String ns, String name, String[] options, int def);
  boolean getAttributeBooleanValue(String ns, String name, boolean def);
  int getAttributeResourceValue(String ns, String name, int def);
  int getAttributeIntValue(String ns, String name, int def);
  int getAttributeUnsignedIntValue(String ns, String name, int def);
  float getAttributeFloatValue(String ns, String name, float def);
  int getAttributeListValue(int index, String[] options, int def);
  boolean getAttributeBooleanValue(int index, boolean def);
  int getAttributeResourceValue(int index, int def);
  int getAttributeIntValue(int index, int def);
  int getAttributeUnsignedIntValue(int index, int def);
  float getAttributeFloatValue(int index, float def);
  String getIdAttribute();
  String getClassAttribute();
  int getIdAttributeResourceValue(int def);
  int getStyleAttribute();
}
