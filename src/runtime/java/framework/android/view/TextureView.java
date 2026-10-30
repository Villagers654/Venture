package android.view;

import android.content.Context;
import android.graphics.SurfaceTexture;
import android.util.AttributeSet;

/**
 * AOSP android.view.TextureView. Venture has no 2D view compositor, so a
 * TextureView never becomes available and its listener is not called.
 */
public class TextureView extends View {
  public interface SurfaceTextureListener {
    void onSurfaceTextureAvailable(SurfaceTexture surface, int width, int height);
    void onSurfaceTextureSizeChanged(SurfaceTexture surface, int width, int height);
    boolean onSurfaceTextureDestroyed(SurfaceTexture surface);
    void onSurfaceTextureUpdated(SurfaceTexture surface);
  }
  private SurfaceTextureListener listener;
  public TextureView(Context context) { super(context); }
  public TextureView(Context context, AttributeSet attrs) { super(context, attrs); }
  public TextureView(Context context, AttributeSet attrs, int defStyleAttr) { super(context, attrs, defStyleAttr); }
  public TextureView(Context context, AttributeSet attrs, int defStyleAttr, int defStyleRes) {
    super(context, attrs, defStyleAttr, defStyleRes);
  }
  public boolean isAvailable() { return false; }
  public SurfaceTexture getSurfaceTexture() { return null; }
  public SurfaceTextureListener getSurfaceTextureListener() { return listener; }
  public void setSurfaceTextureListener(SurfaceTextureListener listener) { this.listener = listener; }
  public void setOpaque(boolean opaque) {}
  public boolean isOpaque() { return true; }
}
