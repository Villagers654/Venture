package android.media;

import android.graphics.Rect;
import java.nio.ByteBuffer;

// AOSP media/java/android/media/Image.java (API 19): a frame's planes as direct
// buffers. MediaCodec.getOutputImage serves YUV_420_888 images; an image is
// invalid once closed or its output buffer is released.
public abstract class Image implements AutoCloseable {
  private Rect cropRect;
  private int dataSpace;
  protected Image() {}
  void throwISEIfImageIsInvalid() {}
  public abstract int getFormat();
  public abstract int getWidth();
  public abstract int getHeight();
  public abstract long getTimestamp();
  public void setTimestamp(long timestamp) { throwISEIfImageIsInvalid(); }
  public int getDataSpace() { throwISEIfImageIsInvalid(); return dataSpace; }
  public void setDataSpace(int dataSpace) { throwISEIfImageIsInvalid(); this.dataSpace = dataSpace; }
  public Rect getCropRect() {
    throwISEIfImageIsInvalid();
    return cropRect == null ? new Rect(0, 0, getWidth(), getHeight()) : new Rect(cropRect);
  }
  public void setCropRect(Rect cropRect) {
    throwISEIfImageIsInvalid();
    if (cropRect != null) {
      cropRect = new Rect(Math.max(cropRect.left, 0), Math.max(cropRect.top, 0),
          Math.min(cropRect.right, getWidth()), Math.min(cropRect.bottom, getHeight()));
      if (cropRect.isEmpty()) cropRect.set(0, 0, 0, 0);
    }
    this.cropRect = cropRect;
  }
  // API 28: images without a buffer (CPU frames) return null.
  public android.hardware.HardwareBuffer getHardwareBuffer() { throwISEIfImageIsInvalid(); return null; }
  public abstract Plane[] getPlanes();
  @Override public abstract void close();
  public abstract static class Plane {
    protected Plane() {}
    public abstract int getRowStride();
    public abstract int getPixelStride();
    public abstract ByteBuffer getBuffer();
  }
}
