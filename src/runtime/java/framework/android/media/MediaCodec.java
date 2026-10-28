package android.media;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** Software decoder using the same bounded host codec as AMediaCodec. */
public final class MediaCodec {
  public static final int BUFFER_FLAG_KEY_FRAME=1, BUFFER_FLAG_SYNC_FRAME=1,
      BUFFER_FLAG_CODEC_CONFIG=2, BUFFER_FLAG_END_OF_STREAM=4;
  public static final int CONFIGURE_FLAG_ENCODE=1, INFO_TRY_AGAIN_LATER=-1,
      INFO_OUTPUT_FORMAT_CHANGED=-2, INFO_OUTPUT_BUFFERS_CHANGED=-3;
  public static final int VIDEO_SCALING_MODE_SCALE_TO_FIT=1,
      VIDEO_SCALING_MODE_SCALE_TO_FIT_WITH_CROPPING=2;
  private long peer;
  private MediaFormat format;
  private final String mime;
  private static native long nativeCreate(String mime);
  private static native int nativeConfigure(long peer, String mime, int width, int height,
      int rate, int channels, byte[] config, android.view.Surface surface);
  private static native long nativeControl(long peer,int operation,long value);
  private static native ByteBuffer nativeBuffer(long peer,boolean input,int index);
  private static native int nativeQueue(long peer,int index,int offset,int size,long pts,int flags);
  private static native int nativeOutput(long peer,long timeout,long[] info);
  private static native int nativeRender(long peer,int index,boolean render,long at);
  private static native int nativeSurface(long peer,android.view.Surface surface);
  private static native void nativeRelease(long peer);
  private MediaCodec(String mime) throws java.io.IOException {
    this.mime=mime;peer=nativeCreate(mime);
    if(peer==0)throw new java.io.IOException("Unsupported decoder: "+mime);
  }
  public static MediaCodec createDecoderByType(String mime) throws java.io.IOException {
    return new MediaCodec(java.util.Objects.requireNonNull(mime));
  }
  public static MediaCodec createEncoderByType(String mime) throws java.io.IOException {
    throw new java.io.IOException("Media encoding unavailable");
  }
  private void live() {if(peer==0)throw new IllegalStateException("Codec released");}
  private static void check(long result) {
    if(result<0)throw new IllegalStateException("MediaCodec error "+result);
  }
  public synchronized void configure(MediaFormat format,android.view.Surface surface,MediaCrypto crypto,int flags) {
    live();java.util.Objects.requireNonNull(format);
    if(crypto!=null || flags!=0)throw new IllegalArgumentException("Unsupported codec configuration");
    var bytes=new java.io.ByteArrayOutputStream();
    for(int i=0;i<3;i++)if(format.containsKey("csd-"+i)) {
      var buffer=format.getByteBuffer("csd-"+i).duplicate();byte[] data=new byte[buffer.remaining()];buffer.get(data);bytes.writeBytes(data);
    }
    check(nativeConfigure(peer,format.getString(MediaFormat.KEY_MIME),format.getInteger("width",0),
        format.getInteger("height",0),format.getInteger("sample-rate",0),format.getInteger("channel-count",0),bytes.toByteArray(),surface));
    this.format=format;
  }
  public synchronized void start(){live();check(nativeControl(peer,0,0));}
  public synchronized void stop(){live();check(nativeControl(peer,1,0));}
  public synchronized void flush(){live();check(nativeControl(peer,2,0));}
  public synchronized void reset(){live();nativeRelease(peer);peer=0;peer=nativeCreate(mime);if(peer==0)throw new IllegalStateException("Codec reset failed");format=null;}
  public synchronized void release(){if(peer!=0)nativeRelease(peer);peer=0;format=null;}
  public int dequeueInputBuffer(long timeoutUs){long id; synchronized(this){live();id=peer;}long result=nativeControl(id,3,timeoutUs);if(result<-1)check(result);return (int)result;}
  public int dequeueOutputBuffer(BufferInfo info,long timeoutUs){long id;synchronized(this){live();id=peer;}long[] values=new long[4];int result=nativeOutput(id,timeoutUs,values);if(result<-3)check(result);if(result>=0)info.set((int)values[0],(int)values[1],values[2],(int)values[3]);return result;}
  public synchronized ByteBuffer getInputBuffer(int index){live();var value=nativeBuffer(peer,true,index);return value==null?null:value.order(ByteOrder.nativeOrder());}
  public synchronized ByteBuffer getOutputBuffer(int index){live();var value=nativeBuffer(peer,false,index);return value==null?null:value.order(ByteOrder.nativeOrder());}
  public ByteBuffer[] getInputBuffers(){return new ByteBuffer[]{getInputBuffer(0)};}
  public ByteBuffer[] getOutputBuffers(){return new ByteBuffer[]{getOutputBuffer(0)};}
  public synchronized void queueInputBuffer(int index,int offset,int size,long pts,int flags){live();if(offset<0 || size<0)throw new IllegalArgumentException();check(nativeQueue(peer,index,offset,size,pts,flags));}
  public synchronized void releaseOutputBuffer(int index,boolean render){live();check(nativeRender(peer,index,render,0));}
  public synchronized void releaseOutputBuffer(int index,long renderTimestampNs){live();check(nativeRender(peer,index,true,renderTimestampNs));}
  public synchronized void setOutputSurface(android.view.Surface surface){live();check(nativeSurface(peer,java.util.Objects.requireNonNull(surface)));}
  public synchronized MediaFormat getOutputFormat(){live();if(format==null)throw new IllegalStateException();
    if(mime.startsWith("video/")) {
      int width=(int)nativeControl(peer,4,0),height=(int)nativeControl(peer,5,0);
      var output=MediaFormat.createVideoFormat("video/raw",width,height);
      output.setInteger("color-format",(int)nativeControl(peer,6,0));output.setInteger("stride",width);output.setInteger("slice-height",height);
      output.setInteger("crop-left",0);output.setInteger("crop-top",0);output.setInteger("crop-right",width-1);output.setInteger("crop-bottom",height-1);return output;
    }
    var output=MediaFormat.createAudioFormat("audio/raw",format.getInteger("sample-rate"),format.getInteger("channel-count"));output.setInteger("pcm-encoding",2);return output;
  }
  public MediaFormat getOutputFormat(int index){return getOutputFormat();}
  public MediaFormat getInputFormat(){live();return format;}
  public void setVideoScalingMode(int mode){live();if(mode!=1 && mode!=2)throw new IllegalArgumentException();}
  public static final class BufferInfo {
    public int offset,size,flags;public long presentationTimeUs;
    public void set(int offset,int size,long pts,int flags){this.offset=offset;this.size=size;presentationTimeUs=pts;this.flags=flags;}
  }
}
