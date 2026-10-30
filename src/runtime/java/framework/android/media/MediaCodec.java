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
  public static final String PARAMETER_KEY_REQUEST_SYNC_FRAME = "request-sync", PARAMETER_KEY_SUSPEND = "drop-input-frames",
      PARAMETER_KEY_VIDEO_BITRATE = "video-bitrate", PARAMETER_KEY_LOW_LATENCY = "low-latency";
  public static final int CRYPTO_MODE_UNENCRYPTED = 0, CRYPTO_MODE_AES_CTR = 1, CRYPTO_MODE_AES_CBC = 2;
  private volatile long peer;
  private MediaFormat format;
  // Asynchronous mode (setCallback): a worker polls the native codec and posts its events to the
  // callback Handler. workerLock is taken before this; flush() pauses the worker until start().
  private final Object workerLock = new Object();
  private Callback callback;
  private android.os.Handler callbackHandler;
  private Thread worker;
  private boolean running, flushed, workerStop;
  private OnFrameRenderedListener frameListener;
  private android.os.Handler frameHandler;
  private volatile long outputPts;
  private final String mime;
  private String name;
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
  // Quest component names (c2.qti.avc.decoder, OMX.google.h264.decoder, ...) by
  // the format they decode. AOSP throws IllegalArgumentException for an unknown name.
  public static MediaCodec createByCodecName(String name) throws java.io.IOException {
    String lower = name.toLowerCase(java.util.Locale.ROOT);
    if (lower.contains("encoder")) throw new java.io.IOException("Media encoding unavailable");
    String mime = lower.contains("avc") || lower.contains("h264") ? "video/avc"
        : lower.contains("hevc") || lower.contains("h265") ? "video/hevc"
        : lower.contains("vp9") ? "video/x-vnd.on2.vp9" : lower.contains("vp8") ? "video/x-vnd.on2.vp8"
        : lower.contains("aac") ? "audio/mp4a-latm" : null;
    if (mime == null) throw new IllegalArgumentException("Unknown codec name: " + name);
    var codec = new MediaCodec(mime);
    codec.name = name;
    return codec;
  }
  public String getName() { return name != null ? name : "c2.android." + mime.substring(mime.indexOf('/') + 1) + ".decoder"; }
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
  public void start(){
    synchronized(workerLock){
      synchronized(this){live();if(!(callback!=null && flushed))check(nativeControl(peer,0,0));}
      if(callback==null)return;
      flushed=false;running=true;
      if(worker==null){worker=new Thread(this::pollAsync,"MediaCodec_looper");worker.setDaemon(true);worker.start();}
      workerLock.notifyAll();
    }
  }
  public void stop(){synchronized(workerLock){running=false;flushed=false;synchronized(this){live();check(nativeControl(peer,1,0));}}}
  public void flush(){
    synchronized(workerLock){
      synchronized(this){live();check(nativeControl(peer,2,0));}
      // AOSP: a flushed codec in asynchronous mode resumes on start().
      if(callback!=null){running=false;flushed=true;}
    }
  }
  public void reset(){
    stopWorker();
    synchronized(this){live();nativeRelease(peer);peer=0;peer=nativeCreate(mime);if(peer==0)throw new IllegalStateException("Codec reset failed");format=null;}
  }
  public void release(){stopWorker();synchronized(this){if(peer!=0)nativeRelease(peer);peer=0;format=null;}}
  private void stopWorker(){
    Thread thread;
    synchronized(workerLock){workerStop=true;running=false;flushed=false;callback=null;thread=worker;worker=null;workerLock.notifyAll();}
    if(thread!=null && thread!=Thread.currentThread())try{thread.join();}catch(InterruptedException e){Thread.currentThread().interrupt();}
    synchronized(workerLock){workerStop=false;}
  }
  public void setCallback(Callback cb,android.os.Handler handler){
    synchronized(workerLock){
      if(running)throw new IllegalStateException("setCallback after start");
      callback=cb;
      if(cb==null){callbackHandler=null;return;}
      var looper=android.os.Looper.myLooper();
      callbackHandler=handler!=null?handler:new android.os.Handler(looper!=null?looper:android.os.Looper.getMainLooper());
    }
  }
  public void setCallback(Callback cb){setCallback(cb,null);}
  private void pollAsync(){
    long[] values=new long[4];
    while(true){
      synchronized(workerLock){
        try{while(!running && !workerStop)workerLock.wait();}catch(InterruptedException e){return;}
        if(workerStop)return;
        final Callback cb=callback;final android.os.Handler handler=callbackHandler;final long id=peer;
        long input=nativeControl(id,3,0);
        if(input>=0){final int index=(int)input;handler.post(()->cb.onInputBufferAvailable(this,index));}
        else if(input<-1){fail(cb,handler,(int)input);continue;}
        int output=nativeOutput(id,input>=0?0:2000,values);
        if(output==INFO_OUTPUT_FORMAT_CHANGED){handler.post(()->{MediaFormat f;try{f=getOutputFormat();}catch(IllegalStateException e){return;}cb.onOutputFormatChanged(this,f);});}
        else if(output>=0){
          var info=new BufferInfo();info.set((int)values[0],(int)values[1],values[2],(int)values[3]);outputPts=values[2];
          final int index=output;handler.post(()->cb.onOutputBufferAvailable(this,index,info));
        }else if(output<-3)fail(cb,handler,output);
      }
    }
  }
  private void fail(Callback cb,android.os.Handler handler,int code){
    running=false;
    var error=new CodecException(code,"MediaCodec error "+code);
    handler.post(()->cb.onError(this,error));
  }
  public int dequeueInputBuffer(long timeoutUs){long id; synchronized(this){live();id=peer;}long result=nativeControl(id,3,timeoutUs);if(result<-1)check(result);return (int)result;}
  public int dequeueOutputBuffer(BufferInfo info,long timeoutUs){long id;synchronized(this){live();id=peer;}long[] values=new long[4];int result=nativeOutput(id,timeoutUs,values);if(result<-3)check(result);if(result>=0){info.set((int)values[0],(int)values[1],values[2],(int)values[3]);outputPts=values[2];}return result;}
  public synchronized ByteBuffer getInputBuffer(int index){live();var value=nativeBuffer(peer,true,index);return value==null?null:value.order(ByteOrder.nativeOrder());}
  public synchronized ByteBuffer getOutputBuffer(int index){live();var value=nativeBuffer(peer,false,index);return value==null?null:value.order(ByteOrder.nativeOrder());}
  public ByteBuffer[] getInputBuffers(){return new ByteBuffer[]{getInputBuffer(0)};}
  public ByteBuffer[] getOutputBuffers(){return new ByteBuffer[]{getOutputBuffer(0)};}
  public synchronized void queueInputBuffer(int index,int offset,int size,long pts,int flags){live();if(offset<0 || size<0)throw new IllegalArgumentException();check(nativeQueue(peer,index,offset,size,pts,flags));}
  public synchronized void releaseOutputBuffer(int index,boolean render){live();check(nativeRender(peer,index,render,0));if(render)frameRendered();}
  public synchronized void releaseOutputBuffer(int index,long renderTimestampNs){live();check(nativeRender(peer,index,true,renderTimestampNs));frameRendered();}
  private void frameRendered(){
    var listener=frameListener;var handler=frameHandler;
    if(listener==null)return;
    long pts=outputPts,now=System.nanoTime();
    handler.post(()->listener.onFrameRendered(this,pts,now));
  }
  public synchronized void setOnFrameRenderedListener(OnFrameRenderedListener listener,android.os.Handler handler){
    frameListener=listener;
    var looper=android.os.Looper.myLooper();
    frameHandler=listener==null?null:handler!=null?handler:new android.os.Handler(looper!=null?looper:android.os.Looper.getMainLooper());
  }
  /** Venture's decoders take no runtime parameters; AOSP ignores keys a codec does not support. */
  public void setParameters(android.os.Bundle params){live();}
  public android.os.PersistableBundle getMetrics(){
    var metrics=new android.os.PersistableBundle();metrics.putString("android.media.mediacodec.codec",getName());
    metrics.putString("android.media.mediacodec.mime",mime);return metrics;
  }
  public void queueSecureInputBuffer(int index,int offset,CryptoInfo info,long pts,int flags){
    live();
    if(info!=null && info.mode==CRYPTO_MODE_UNENCRYPTED){
      int size=0;
      for(int i=0;i<info.numSubSamples;i++)size+=(info.numBytesOfClearData!=null?info.numBytesOfClearData[i]:0)
          +(info.numBytesOfEncryptedData!=null?info.numBytesOfEncryptedData[i]:0);
      queueInputBuffer(index,offset,size,pts,flags);return;
    }
    throw new CryptoException(CryptoException.ERROR_UNSUPPORTED_OPERATION,"Protected content decoding unavailable");
  }
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
  public abstract static class Callback {
    public abstract void onInputBufferAvailable(MediaCodec codec,int index);
    public abstract void onOutputBufferAvailable(MediaCodec codec,int index,BufferInfo info);
    public abstract void onError(MediaCodec codec,CodecException e);
    public abstract void onOutputFormatChanged(MediaCodec codec,MediaFormat format);
  }
  public interface OnFrameRenderedListener {
    void onFrameRendered(MediaCodec codec,long presentationTimeUs,long nanoTime);
  }
  public static final class CodecException extends IllegalStateException {
    public static final int ERROR_INSUFFICIENT_RESOURCE=1100,ERROR_RECLAIMED=1101;
    private final int errorCode;
    CodecException(int errorCode,String detail){super(detail);this.errorCode=errorCode;}
    public boolean isTransient(){return false;}
    public boolean isRecoverable(){return false;}
    public int getErrorCode(){return errorCode;}
    public String getDiagnosticInfo(){return "android.media.MediaCodec.error_"+(errorCode<0?"neg_":"")+Math.abs(errorCode);}
  }
  public static final class CryptoException extends RuntimeException {
    public static final int ERROR_NO_KEY=1,ERROR_KEY_EXPIRED=2,ERROR_RESOURCE_BUSY=3,ERROR_INSUFFICIENT_OUTPUT_PROTECTION=4,
        ERROR_SESSION_NOT_OPENED=5,ERROR_UNSUPPORTED_OPERATION=6;
    private final int errorCode;
    public CryptoException(int errorCode,String detailMessage){super(detailMessage);this.errorCode=errorCode;}
    public int getErrorCode(){return errorCode;}
  }
  public static final class CryptoInfo {
    public int numSubSamples,mode;
    public int[] numBytesOfClearData,numBytesOfEncryptedData;
    public byte[] key,iv;
    private Pattern pattern=new Pattern(0,0);
    public CryptoInfo(){}
    public void set(int numSubSamples,int[] clear,int[] encrypted,byte[] key,byte[] iv,int mode){
      this.numSubSamples=numSubSamples;numBytesOfClearData=clear;numBytesOfEncryptedData=encrypted;this.key=key;this.iv=iv;this.mode=mode;
    }
    public void setPattern(Pattern value){pattern=value!=null?value:new Pattern(0,0);}
    public static final class Pattern {
      private int encryptBlocks,skipBlocks;
      public Pattern(int blocksToEncrypt,int blocksToSkip){set(blocksToEncrypt,blocksToSkip);}
      public void set(int blocksToEncrypt,int blocksToSkip){encryptBlocks=blocksToEncrypt;skipBlocks=blocksToSkip;}
      public int getEncryptBlocks(){return encryptBlocks;}
      public int getSkipBlocks(){return skipBlocks;}
    }
  }
  public static final class BufferInfo {
    public int offset,size,flags;public long presentationTimeUs;
    public void set(int offset,int size,long pts,int flags){this.offset=offset;this.size=size;presentationTimeUs=pts;this.flags=flags;}
  }
}
