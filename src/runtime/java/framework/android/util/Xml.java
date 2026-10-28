package android.util;
public final class Xml {
  public static org.xmlpull.v1.XmlPullParser newPullParser() {
    return new org.kxml2.io.KXmlParser();
  }
  public static org.xmlpull.v1.XmlSerializer newSerializer() {
    return new org.kxml2.io.KXmlSerializer();
  }
  public static AttributeSet asAttributeSet(org.xmlpull.v1.XmlPullParser parser) {
    if (parser instanceof AttributeSet attributes)
      return attributes;
    throw new UnsupportedOperationException(
        "Use Resources.getXml for compiled resource attributes");
  }
}
