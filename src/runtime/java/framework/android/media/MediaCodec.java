package android.media;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/** Software decoder using the same bounded host codec as AMediaCodec. */
public final class MediaCodec {
  public static final int BUFFER_FLAG_KEY_FRAME=1, BUFFER_FLAG_SYNC_FRAME=1,
      BUFFER_FLAG_CODEC_CONFIG=2, BUFFER_FLAG_END_OF_STREAM=4, BUFFER_FLAG_PARTIAL_FRAME=8, BUFFER_FLAG_MUXER_DATA=16;
  public static final int CONFIGURE_FLAG_ENCODE=1, CONFIGURE_FLAG_USE_BLOCK_MODEL=2, INFO_TRY_AGAIN_LATER=-1,
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
  private volatile Callback callback;
  private android.os.Handler callbackHandler;
  private Thread worker;
  private boolean running, flushed, workerStop;
  private OnFrameRenderedListener frameListener;
  private android.os.Handler frameHandler;
  private volatile long outputPts;
  private final String mime;
  private String name;
  private CodecImage outputImage;
  private ByteBuffer imageBuffer;
  // Executing state (start() until stop/reset/release), for the legacy buffer-array getters.
  private boolean executing;
  // ByteBuffer video output requested as I420 (COLOR_FormatYUV420Planar or PackedPlanar);
  // the host decoder's NV12 frame is converted into planarBuffer.
  private int planarFormat;
  private ByteBuffer planarBuffer;
  // getOutputBuffers() callers index one array for the codec's lifetime: each dequeued output
  // is copied into its buffer. A larger frame first reports INFO_OUTPUT_BUFFERS_CHANGED and
  // holds the dequeued output for the next dequeueOutputBuffer.
  private ByteBuffer[] legacyOutputs;
  private long[] heldOutput;
  private int heldIndex=-1;
  private static native long nativeCreate(String mime);
  private static native int nativeConfigure(long peer, String mime, int width, int height,
      int rate, int channels, byte[] config, android.view.Surface surface);
  private static native long nativeControl(long peer,int operation,long value);
  private static native ByteBuffer nativeBuffer(long peer,boolean input,int index);
  private static native int nativeImage(long peer,int index,ByteBuffer target);
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
  public String getName() {
    if (name != null) return name;
    var listed = MediaCodecList.decoderFor(mime);
    return listed != null ? listed.getName() : "c2.android." + mime.substring(mime.indexOf('/') + 1) + ".decoder";
  }
  /** The MediaCodecList entry this codec was created from; a codec created by another name is its alias. */
  public MediaCodecInfo getCodecInfo() {
    live();
    var listed = MediaCodecList.decoderFor(mime);
    if (listed == null) return new MediaCodecInfo(getName(), mime);
    return name == null || name.equals(listed.getName()) ? listed
        : new MediaCodecInfo(name, listed.getSupportedTypes()[0], listed.getName());
  }
  public String getCanonicalName() { return getCodecInfo().getCanonicalName(); }
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
    int color=format.getInteger(MediaFormat.KEY_COLOR_FORMAT,0);
    planarFormat=surface==null && mime.startsWith("video/") && (color==MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Planar
        || color==MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420PackedPlanar)?color:0;
    legacyOutputs=null;heldIndex=-1;
  }
  public void start(){
    synchronized(workerLock){
      synchronized(this){live();if(!(callback!=null && flushed))check(nativeControl(peer,0,0));executing=true;}
      if(callback==null)return;
      flushed=false;running=true;
      if(worker==null){worker=new Thread(this::pollAsync,"MediaCodec_looper");worker.setDaemon(true);worker.start();}
      workerLock.notifyAll();
    }
  }
  public void stop(){synchronized(workerLock){running=false;flushed=false;synchronized(this){live();dropImage();executing=false;legacyOutputs=null;heldIndex=-1;check(nativeControl(peer,1,0));}}}
  public void flush(){
    synchronized(workerLock){
      synchronized(this){live();dropImage();heldIndex=-1;check(nativeControl(peer,2,0));}
      // AOSP: a flushed codec in asynchronous mode resumes on start().
      if(callback!=null){running=false;flushed=true;}
    }
  }
  public void reset(){
    stopWorker();
    synchronized(this){live();dropImage();executing=false;legacyOutputs=null;heldIndex=-1;planarFormat=0;
      nativeRelease(peer);peer=0;peer=nativeCreate(mime);if(peer==0)throw new IllegalStateException("Codec reset failed");format=null;}
  }
  public void release(){stopWorker();synchronized(this){dropImage();executing=false;legacyOutputs=null;heldIndex=-1;if(peer!=0)nativeRelease(peer);peer=0;format=null;}}
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
          if(values[1]>0 && planarFormat!=0)values[1]=planarSize();
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
  public int dequeueOutputBuffer(BufferInfo info,long timeoutUs){
    long id;
    synchronized(this){
      live();id=peer;
      if(heldIndex>=0){int index=heldIndex;heldIndex=-1;return deliverOutput(index,heldOutput,info);}
    }
    long[] values=new long[4];int result=nativeOutput(id,timeoutUs,values);if(result<-3)check(result);
    if(result<0)return result;
    synchronized(this){live();return deliverOutput(result,values,info);}
  }
  // Sizes a planar frame, and copies the output into the legacy array when a caller holds one.
  private int deliverOutput(int index,long[] values,BufferInfo info){
    if(values[1]>0 && planarFormat!=0)values[1]=planarSize();
    if(legacyOutputs!=null && values[1]>0) {
      var data=outputData(index);
      if(data!=null) {
        var target=legacyOutputs[0];
        if(target==null || target.capacity()<data.remaining()) {
          legacyOutputs=new ByteBuffer[]{ByteBuffer.allocateDirect(data.remaining()).order(ByteOrder.nativeOrder())};
          heldIndex=index;heldOutput=values;return INFO_OUTPUT_BUFFERS_CHANGED;
        }
        target.clear();target.put(data);
        target.limit((int)Math.min(target.capacity(),values[0]+values[1])).position((int)Math.min(values[0],target.limit()));
      }
    }
    info.set((int)values[0],(int)values[1],values[2],(int)values[3]);outputPts=values[2];return index;
  }
  private int planarSize(){
    int width=(int)nativeControl(peer,4,0),height=(int)nativeControl(peer,5,0);
    return width<=0 || height<=0?0:width*height+2*((width+1)/2)*((height+1)/2);
  }
  // The output buffer's bytes in the configured layout: the host NV12 or audio data, or I420.
  private ByteBuffer outputData(int index){
    if(planarFormat==0){var value=nativeBuffer(peer,false,index);return value==null?null:value.order(ByteOrder.nativeOrder());}
    int size=planarSize();if(size==0)return null;
    if(planarBuffer==null || planarBuffer.capacity()!=size)planarBuffer=ByteBuffer.allocateDirect(size).order(ByteOrder.nativeOrder());
    if(nativeImage(peer,index,planarBuffer)!=0)return null;
    return planarBuffer.duplicate().clear().order(ByteOrder.nativeOrder());
  }
  public synchronized ByteBuffer getInputBuffer(int index){live();var value=nativeBuffer(peer,true,index);return value==null?null:value.order(ByteOrder.nativeOrder());}
  public synchronized ByteBuffer getOutputBuffer(int index){live();return outputData(index);}
  // AOSP returns null when a buffer cannot be served as an Image (input, surface or audio output).
  public Image getInputImage(int index){live();return null;}
  public synchronized Image getOutputImage(int index){
    live();if(!mime.startsWith("video/"))return null;
    int width=(int)nativeControl(peer,4,0),height=(int)nativeControl(peer,5,0);
    if(width<=0 || height<=0)return null;
    int size=width*height+2*((width+1)/2)*((height+1)/2);
    dropImage();
    if(imageBuffer==null || imageBuffer.capacity()!=size)imageBuffer=ByteBuffer.allocateDirect(size);
    if(nativeImage(peer,index,imageBuffer)!=0)return null;
    return outputImage=new CodecImage(imageBuffer,width,height,outputPts*1000);
  }
  private void dropImage(){if(outputImage!=null)outputImage.close();outputImage=null;}
  // One output frame as YUV_420_888 (the AImageReader layout): a full Y plane, then quarter-size
  // U and V planes. Valid until its output buffer is released; the next image reuses the buffer.
  private static final class CodecImage extends Image {
    private final int width,height;
    private long timestamp;
    private Plane[] planes;
    CodecImage(ByteBuffer data,int width,int height,long timestamp){
      this.width=width;this.height=height;this.timestamp=timestamp;
      int luma=width*height,chromaWidth=(width+1)/2,chroma=chromaWidth*((height+1)/2);
      planes=new Plane[]{plane(data,0,luma,width),plane(data,luma,chroma,chromaWidth),plane(data,luma+chroma,chroma,chromaWidth)};
    }
    private static Plane plane(ByteBuffer data,int offset,int length,int rowStride){
      var view=data.duplicate();view.clear();view.position(offset).limit(offset+length);
      var buffer=view.slice();
      return new Plane(){
        @Override public int getRowStride(){return rowStride;}
        @Override public int getPixelStride(){return 1;}
        @Override public ByteBuffer getBuffer(){return buffer;}
      };
    }
    @Override protected void throwISEIfImageIsInvalid(){if(planes==null)throw new IllegalStateException("Image is already closed");}
    // AOSP Image (a 2D process uses it): an image from a codec is not scaled or transformed.
    public int getScalingMode(){return 0;}
    public int getTransform(){return 0;}
    @Override public int getFormat(){throwISEIfImageIsInvalid();return android.graphics.ImageFormat.YUV_420_888;}
    @Override public int getWidth(){throwISEIfImageIsInvalid();return width;}
    @Override public int getHeight(){throwISEIfImageIsInvalid();return height;}
    @Override public long getTimestamp(){throwISEIfImageIsInvalid();return timestamp;}
    @Override public void setTimestamp(long timestamp){throwISEIfImageIsInvalid();this.timestamp=timestamp;}
    @Override public Plane[] getPlanes(){throwISEIfImageIsInvalid();return planes.clone();}
    @Override public void close(){planes=null;}
  }
  // AOSP: the legacy arrays exist only while Executing in synchronous mode. The codec has one
  // input and one output buffer (index 0); the input array aliases the codec's input memory.
  private void legacyArrays(){
    live();
    if(callback!=null)throw new IllegalStateException("buffer arrays in asynchronous mode");
    if(!executing)throw new IllegalStateException("codec is not executing");
  }
  public synchronized ByteBuffer[] getInputBuffers(){
    legacyArrays();
    var value=nativeBuffer(peer,true,0);
    return new ByteBuffer[]{value==null?null:value.order(ByteOrder.nativeOrder())};
  }
  public synchronized ByteBuffer[] getOutputBuffers(){
    legacyArrays();
    // Surface output has no CPU buffers (AOSP returns null entries).
    if(legacyOutputs==null)legacyOutputs=new ByteBuffer[]{null};
    if(legacyOutputs[0]==null && !isSurfaceOutput()) {
      int size=!mime.startsWith("video/")?65536:planarFormat!=0?planarSize():nv12Size();
      if(size>0)legacyOutputs[0]=ByteBuffer.allocateDirect(size).order(ByteOrder.nativeOrder());
    }
    return legacyOutputs.clone();
  }
  private boolean isSurfaceOutput(){return nativeControl(peer,6,0)==MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface;}
  private int nv12Size(){
    int width=(int)nativeControl(peer,4,0),height=(int)nativeControl(peer,5,0);
    return width<=0 || height<=0?0:((width+1)&~1)*((height+1)&~1)*3/2;
  }
  public synchronized void queueInputBuffer(int index,int offset,int size,long pts,int flags){live();if(offset<0 || size<0)throw new IllegalArgumentException();check(nativeQueue(peer,index,offset,size,pts,flags));}
  public synchronized void releaseOutputBuffer(int index,boolean render){live();dropImage();check(nativeRender(peer,index,render,0));if(render)frameRendered();}
  public synchronized void releaseOutputBuffer(int index,long renderTimestampNs){live();dropImage();check(nativeRender(peer,index,true,renderTimestampNs));frameRendered();}
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
      if(planarFormat!=0){output.setInteger("color-format",planarFormat);output.setInteger("stride",width);output.setInteger("slice-height",height);}
      else{output.setInteger("color-format",(int)nativeControl(peer,6,0));output.setInteger("stride",(width+1)&~1);output.setInteger("slice-height",(height+1)&~1);}
      output.setInteger("crop-left",0);output.setInteger("crop-top",0);output.setInteger("crop-right",width-1);output.setInteger("crop-bottom",height-1);return output;
    }
    var output=MediaFormat.createAudioFormat("audio/raw",format.getInteger("sample-rate"),format.getInteger("channel-count"));output.setInteger("pcm-encoding",2);return output;
  }
  public MediaFormat getOutputFormat(int index){return getOutputFormat();}
  public MediaFormat getInputFormat(){live();return format;}
  public void setVideoScalingMode(int mode){live();if(mode!=1 && mode!=2)throw new IllegalArgumentException();}
  // Encoder input: AOSP throws IllegalStateException unless configured with CONFIGURE_FLAG_ENCODE,
  // which configure() rejects (there are no encoders).
  public android.view.Surface createInputSurface(){live();throw new IllegalStateException("codec is not an encoder");}
  public void setInputSurface(android.view.Surface surface){live();throw new IllegalStateException("codec is not an encoder");}
  public void signalEndOfInputStream(){live();throw new IllegalStateException("codec is not an encoder");}
  // Vendor parameters (API 31): the host decoders define none.
  public java.util.List<String> getSupportedVendorParameters(){live();return new java.util.ArrayList<>();}
  public ParameterDescriptor getParameterDescriptor(String name){live();return null;}
  public void subscribeToVendorParameters(java.util.List<String> names){live();}
  public void unsubscribeFromVendorParameters(java.util.List<String> names){live();}
  public static class ParameterDescriptor {
    private final String name;private final int type;
    ParameterDescriptor(String name,int type){this.name=name;this.type=type;}
    public String getName(){return name;}
    public int getType(){return type;}
  }
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
