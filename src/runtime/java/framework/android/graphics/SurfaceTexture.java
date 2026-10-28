package android.graphics;

/** Latest-frame consumer backed by Venture's shared native-window queue. */
public class SurfaceTexture {
  private long handle,serial,timestamp;
  private int texture;
  private boolean detached;
  private volatile boolean released;
  // Producers must never acquire the public consumer monitor: release can
  // join a producer while its caller holds that monitor.
  private final Object eventLock=new Object();
  private OnFrameAvailableListener listener;
  private android.os.Handler handler;
  private long listenerGeneration;
  private boolean callbackPending;
  private final android.os.Looper creator=android.os.Looper.myLooper();
  private native long nativeCreate();
  private static native long nativeAcquire(long handle);
  private static native void nativeRelease(long handle);
  private static native void nativeSize(long handle,int width,int height);
  private static native long[] nativeUpdate(long handle,int texture,long serial);
  public SurfaceTexture(int texture){this(texture,false);}
  public SurfaceTexture(int texture,boolean singleBufferMode){this.texture=texture;handle=nativeCreate();if(handle==0)throw new IllegalStateException("SurfaceTexture creation failed");}
  public SurfaceTexture(boolean singleBufferMode){this(0,singleBufferMode);detached=true;}
  private void live(){if(released)throw new IllegalStateException("SurfaceTexture released");}
  public synchronized long ventureAcquireWindow(){live();return nativeAcquire(handle);}
  public synchronized void setDefaultBufferSize(int width,int height){live();if(width<=0 || height<=0)throw new IllegalArgumentException();nativeSize(handle,width,height);}
  public synchronized void updateTexImage(){live();if(detached)throw new IllegalStateException("SurfaceTexture detached");var result=nativeUpdate(handle,texture,serial);if(result!=null){serial=result[0];timestamp=result[1];}}
  public synchronized long getTimestamp(){live();return timestamp;}
  public synchronized void getTransformMatrix(float[] matrix){live();if(matrix.length<16)throw new IllegalArgumentException();java.util.Arrays.fill(matrix,0,16,0);matrix[0]=matrix[10]=matrix[15]=1;matrix[5]=-1;matrix[13]=1;}
  public synchronized void detachFromGLContext(){live();if(detached)throw new IllegalStateException("Already detached");detached=true;texture=0;serial=0;}
  public synchronized void attachToGLContext(int texture){live();if(!detached)throw new IllegalStateException("Already attached");this.texture=texture;detached=false;serial=0;}
  public synchronized void releaseTexImage(){live();}
  public synchronized boolean isReleased(){return released;}
  public synchronized void release(){if(released)return;synchronized(eventLock){released=true;listener=null;++listenerGeneration;}nativeRelease(handle);handle=0;}
  public void setOnFrameAvailableListener(OnFrameAvailableListener listener){setOnFrameAvailableListener(listener,null);}
  public synchronized void setOnFrameAvailableListener(OnFrameAvailableListener listener,android.os.Handler handler){
    live();
    var looper=creator!=null?creator:android.os.Looper.getMainLooper();
    synchronized(eventLock){this.listener=listener;++listenerGeneration;callbackPending=false;
      this.handler=handler!=null?handler:looper==null?null:new android.os.Handler(looper);}
  }
  private void postFrameAvailable(){
    final android.os.Handler target;final long token;
    synchronized(eventLock){if(released || listener==null || handler==null || callbackPending)return;callbackPending=true;target=handler;token=listenerGeneration;}
    target.post(()->{OnFrameAvailableListener callback;synchronized(eventLock){if(released || token!=listenerGeneration)return;callbackPending=false;callback=listener;}if(callback!=null)callback.onFrameAvailable(this);});
  }
  public interface OnFrameAvailableListener {void onFrameAvailable(SurfaceTexture surfaceTexture);}
}
