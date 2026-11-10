package android.content.pm;
public class PackageItemInfo {
  public String name, packageName;
  public int labelRes, icon;
  public CharSequence nonLocalizedLabel;
  public android.os.Bundle metaData = new android.os.Bundle();
  public CharSequence loadLabel(PackageManager pm) {
    return nonLocalizedLabel == null ? name : nonLocalizedLabel;
  }
  // AOSP: the XML resource a meta-data entry names through android:resource, or null.
  // androidx FileProvider reads its paths this way (Iron Strike).
  public android.content.res.XmlResourceParser loadXmlMetaData(PackageManager pm, String name) {
    if (metaData == null) return null;
    int id = metaData.getInt(name);
    return id == 0 ? null : pm.getXml(packageName, id, null);
  }
}
