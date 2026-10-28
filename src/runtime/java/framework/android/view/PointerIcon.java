package android.view;

import android.graphics.Bitmap;

public final class PointerIcon {
  public static final int TYPE_CUSTOM = -1;
  final Bitmap bitmap;
  final float hotSpotX, hotSpotY;

  private PointerIcon(Bitmap bitmap, float hotSpotX, float hotSpotY) {
    this.bitmap = bitmap;
    this.hotSpotX = hotSpotX;
    this.hotSpotY = hotSpotY;
  }

  public static PointerIcon create(Bitmap bitmap, float hotSpotX, float hotSpotY) {
    // AOSP core/java/android/view/PointerIcon.java: custom icons retain their
    // bitmap and require the hotspot to be inside its bounds.
    if (bitmap == null)
      throw new IllegalArgumentException("bitmap must not be null");
    if (!(hotSpotX >= 0 && hotSpotX < bitmap.getWidth()
        && hotSpotY >= 0 && hotSpotY < bitmap.getHeight()))
      throw new IllegalArgumentException("Pointer hotspot outside bitmap");
    return new PointerIcon(bitmap, hotSpotX, hotSpotY);
  }

  public int getType() {
    return TYPE_CUSTOM;
  }
}
