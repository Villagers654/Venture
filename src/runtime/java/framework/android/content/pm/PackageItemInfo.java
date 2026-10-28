package android.content.pm;
public class PackageItemInfo {
  public String name, packageName;
  public int labelRes, icon;
  public CharSequence nonLocalizedLabel;
  public android.os.Bundle metaData = new android.os.Bundle();
  public CharSequence loadLabel(PackageManager pm) {
    return nonLocalizedLabel == null ? name : nonLocalizedLabel;
  }
}
