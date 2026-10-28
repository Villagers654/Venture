package android.graphics.drawable;
public class XmlDrawable extends Drawable {
  public final com.venture.runtime.BinaryResources.Node definition;
  public XmlDrawable(com.venture.runtime.BinaryResources.Node root) {
    definition = root;
  }
}
