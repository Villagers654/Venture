import android.media.MediaPlayer;
import android.os.Handler;
import android.os.HandlerThread;
import java.io.FileInputStream;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** Integration fixture: actual JNI media bridge, Looper and authored tone. */
public class MediaFlow {
  // Publication runs on the decoder worker. A consumer may hold its texture
  // monitor while joining that worker during release; publication must finish.
  private static String callbackMonitor() {
    var texture=new android.graphics.SurfaceTexture(true);
    HandlerThread events=new HandlerThread("surface-release-test");events.start();
    CountDownLatch unblock=new CountDownLatch(1),eventEntered=new CountDownLatch(1);
    CountDownLatch entered=new CountDownLatch(1),posted=new CountDownLatch(1);
    AtomicInteger callbacks=new AtomicInteger();
    var failure=new java.util.concurrent.atomic.AtomicReference<Throwable>();
    Thread producer=null;
    try {
      Handler handler=new Handler(events.getLooper());
      handler.post(()->{eventEntered.countDown();try{unblock.await();}catch(InterruptedException e){Thread.currentThread().interrupt();}});
      if(!eventEntered.await(2,TimeUnit.SECONDS))return "surface event thread timeout";
      texture.setOnFrameAvailableListener(t->callbacks.incrementAndGet(),handler);
      var notify=texture.getClass().getDeclaredMethod("postFrameAvailable");notify.setAccessible(true);
      producer=new Thread(()->{entered.countDown();try{notify.invoke(texture);}catch(Throwable e){failure.set(e);}finally{posted.countDown();}},"surface-producer-test");
      synchronized(texture) {
        producer.start();
        if(!entered.await(2,TimeUnit.SECONDS))return "surface producer start timeout";
        if(!posted.await(2,TimeUnit.SECONDS))return "surface producer waits for consumer monitor";
        if(failure.get()!=null)return "surface producer failure: "+failure.get();
        texture.release(); // queued callback must be invalidated by release
      }
      unblock.countDown();
      CountDownLatch drained=new CountDownLatch(1);handler.post(drained::countDown);
      if(!drained.await(2,TimeUnit.SECONDS))return "surface callback drain timeout";
      if(callbacks.get()!=0)return "surface callback after release";
      return null;
    } catch(Throwable e){return "surface monitor: "+e;}
    finally {unblock.countDown();if(producer!=null)try{producer.join(2000);}catch(InterruptedException e){Thread.currentThread().interrupt();}texture.release();events.quitSafely();try{events.join(2000);}catch(InterruptedException e){Thread.currentThread().interrupt();}}
  }
  public static String codec(byte[] config, byte[][] packets, long[] times) {
    android.media.MediaCodec codec=null;
    var texture=new android.graphics.SurfaceTexture(true);
    var replacement=new android.graphics.SurfaceTexture(true);
    var surface=new android.view.Surface(texture);var second=new android.view.Surface(replacement);
    HandlerThread callbackThread=new HandlerThread("video-frame-listener");callbackThread.start();
    AtomicInteger callbacks=new AtomicInteger();
    texture.setOnFrameAvailableListener(t->callbacks.incrementAndGet(),new Handler(callbackThread.getLooper()));
    replacement.setOnFrameAvailableListener(t->callbacks.incrementAndGet(),new Handler(callbackThread.getLooper()));
    try {
      var capabilities=Class.forName("android.media.MediaCodecInfo$CodecCapabilities");
      if(capabilities.getField("COLOR_FormatSurface").getInt(null)!=0x7f000789 || capabilities.getField("COLOR_Format32bitABGR8888").getInt(null)!=0x7f00a000)throw new AssertionError("codec color constants");
      codec=android.media.MediaCodec.createDecoderByType("video/avc");
      var format=android.media.MediaFormat.createVideoFormat("video/avc",128,72);
      format.setByteBuffer("csd-0",java.nio.ByteBuffer.wrap(config));codec.configure(format,surface,null,0);codec.start();
      if(codec.getOutputFormat().getInteger("color-format")!=0x7f000789)throw new AssertionError("Surface format");
      var info=new android.media.MediaCodec.BufferInfo();int input=0,frames=0;boolean queuedEos=false,eos=false;
      long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(5);
      while(!eos && System.nanoTime()<deadline) {
        int index=queuedEos?-1:codec.dequeueInputBuffer(1000);
        if(index>=0) {
          var buffer=codec.getInputBuffer(index);buffer.clear();
          if(input<packets.length){buffer.put(packets[input]);codec.queueInputBuffer(index,0,packets[input].length,times[input],0);input++;}
          else {codec.queueInputBuffer(index,0,0,1000000,4);queuedEos=true;}
        }
        index=codec.dequeueOutputBuffer(info,1000);
        if(index>=0) {
          eos=(info.flags&4)!=0;
          if(!eos){frames++;if(info.size!=128*72*4)throw new AssertionError("frame size");if(frames==6)codec.setOutputSurface(second);}
          codec.releaseOutputBuffer(index,true);
        }
      }
      if(!eos || frames!=12)return "codec EOS/frame count";
      while(callbacks.get()==0 && System.nanoTime()<deadline)Thread.sleep(1);
      if(callbacks.get()==0)return "no SurfaceTexture frame callback";
      codec.flush();codec.stop();codec.release();codec=null;
      return null;
    } catch(Throwable e){return "video codec: "+e;}
    finally {if(codec!=null)codec.release();surface.release();second.release();texture.release();replacement.release();callbackThread.quitSafely();try{callbackThread.join(1000);}catch(InterruptedException e){Thread.currentThread().interrupt();}}
  }
  // Unreal's MediaPlayer14 reads tracks through the Java MediaExtractor and
  // A decoder without a Surface serves each output buffer as a YUV_420_888 Image
  // (CRIWARE Mana reads its movie frames this way), invalid once released.
  // MediaCodec renders into an ImageReader's Surface (Unreal's Electra player
  // on Quest): one onImageAvailable per frame on the listener's Handler, CPU
  // planes for RGBA readers and an AHardwareBuffer for PRIVATE ones.
  public static String imageReader(byte[] config, byte[][] packets, long[] times, int format) {
    android.media.MediaCodec codec=null;
    var reader=android.media.ImageReader.newInstance(128,72,format,3);
    HandlerThread events=new HandlerThread("image-reader-test");events.start();
    var images=new java.util.concurrent.LinkedBlockingQueue<Object>();
    try {
      if(reader.getSurface()==null || !reader.getSurface().isValid())return "reader surface";
      reader.setOnImageAvailableListener(r->{
        try {
          var image=r.acquireNextImage();
          if(image==null){images.add("no image after onImageAvailable");return;}
          try {
            if(image.getWidth()!=128 || image.getHeight()!=72 || image.getFormat()!=format){images.add("image size/format");return;}
            var buffer=image.getHardwareBuffer();
            if(format==android.graphics.ImageFormat.PRIVATE) {
              if(buffer==null){images.add("no hardware buffer");return;}
              if(buffer.getWidth()!=128 || buffer.getHeight()!=72 || buffer.getLayers()!=1){images.add("buffer size");return;}
              if((buffer.getUsage()&android.hardware.HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE)==0){images.add("buffer usage");return;}
              buffer.close();
              if(!buffer.isClosed()){images.add("buffer close");return;}
              images.add(Integer.valueOf(0));
            } else {
              var planes=image.getPlanes();
              if(planes.length!=1 || planes[0].getPixelStride()!=4 || planes[0].getRowStride()!=128*4){images.add("rgba plane");return;}
              var pixels=planes[0].getBuffer();
              if(pixels.remaining()!=128*72*4){images.add("rgba size");return;}
              int low=255,high=0;
              for(int n=0;n<pixels.remaining();n+=4){int v=pixels.get(n)&0xff;low=Math.min(low,v);high=Math.max(high,v);}
              images.add(Integer.valueOf(high-low));
            }
          } finally {image.close();}
          try{image.getPlanes();images.add("image valid after close");}catch(IllegalStateException expected){}
        } catch(Throwable e){images.add("listener: "+e);}
      },new Handler(events.getLooper()));
      codec=android.media.MediaCodec.createDecoderByType("video/avc");
      var mediaFormat=android.media.MediaFormat.createVideoFormat("video/avc",128,72);
      mediaFormat.setByteBuffer("csd-0",java.nio.ByteBuffer.wrap(config));codec.configure(mediaFormat,reader.getSurface(),null,0);codec.start();
      var info=new android.media.MediaCodec.BufferInfo();int input=0,frames=0;boolean queuedEos=false,eos=false;
      long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(5);
      int range=0;
      while(!eos && System.nanoTime()<deadline) {
        int index=queuedEos?-1:codec.dequeueInputBuffer(1000);
        if(index>=0) {
          var buffer=codec.getInputBuffer(index);buffer.clear();
          if(input<packets.length){buffer.put(packets[input]);codec.queueInputBuffer(index,0,packets[input].length,times[input],0);input++;}
          else {codec.queueInputBuffer(index,0,0,1000000,4);queuedEos=true;}
        }
        index=codec.dequeueOutputBuffer(info,1000);
        if(index<0)continue;
        eos=(info.flags&4)!=0;
        codec.releaseOutputBuffer(index,!eos);
        if(eos)break;
        // Wait for this frame's image before rendering the next: a reader drops
        // its oldest frame when full, as an asynchronous BufferQueue does.
        var result=images.poll(2,TimeUnit.SECONDS);
        if(result==null)return "no onImageAvailable for frame "+frames;
        if(result instanceof String)return (String)result;
        range=Math.max(range,(Integer)result);frames++;
      }
      if(!eos || frames!=12)return "reader EOS/frame count "+frames;
      if(format!=android.graphics.ImageFormat.PRIVATE && range<32)return "rgba range "+range;
      reader.close();
      try{reader.acquireNextImage();return "acquire after close";}catch(IllegalStateException expected){}
      return null;
    } catch(Throwable e){return "image reader: "+e;}
    finally {if(codec!=null)codec.release();reader.close();events.quitSafely();try{events.join(2000);}catch(InterruptedException e){Thread.currentThread().interrupt();}}
  }
  public static String image(byte[] config, byte[][] packets, long[] times) {
    android.media.MediaCodec codec=null;
    try {
      codec=android.media.MediaCodec.createDecoderByType("video/avc");
      var format=android.media.MediaFormat.createVideoFormat("video/avc",128,72);
      format.setInteger("color-format",android.media.MediaCodecInfo.CodecCapabilities.COLOR_FormatYUV420Flexible);
      format.setByteBuffer("csd-0",java.nio.ByteBuffer.wrap(config));codec.configure(format,null,null,0);codec.start();
      if(codec.getInputImage(0)!=null)return "input image";
      var info=new android.media.MediaCodec.BufferInfo();int input=0,frames=0,low=255,high=0;boolean queuedEos=false,eos=false;
      long deadline=System.nanoTime()+TimeUnit.SECONDS.toNanos(5);
      while(!eos && System.nanoTime()<deadline) {
        int index=queuedEos?-1:codec.dequeueInputBuffer(1000);
        if(index>=0) {
          var buffer=codec.getInputBuffer(index);buffer.clear();
          if(input<packets.length){buffer.put(packets[input]);codec.queueInputBuffer(index,0,packets[input].length,times[input],0);input++;}
          else {codec.queueInputBuffer(index,0,0,1000000,4);queuedEos=true;}
        }
        index=codec.dequeueOutputBuffer(info,1000);
        if(index<0)continue;
        eos=(info.flags&4)!=0;
        if(!eos) {
          frames++;
          var image=codec.getOutputImage(index);
          if(image==null)return "no output image";
          if(image.getFormat()!=android.graphics.ImageFormat.YUV_420_888 || image.getWidth()!=128 || image.getHeight()!=72)return "image format";
          if(image.getTimestamp()!=info.presentationTimeUs*1000)return "image timestamp";
          var crop=image.getCropRect();if(crop.left!=0 || crop.top!=0 || crop.right!=128 || crop.bottom!=72)return "image crop";
          var planes=image.getPlanes();if(planes.length!=3)return "plane count";
          int[] rows={128,64,64},sizes={128*72,64*36,64*36};
          for(int p=0;p<3;++p)
            if(planes[p].getPixelStride()!=1 || planes[p].getRowStride()!=rows[p] || planes[p].getBuffer().remaining()!=sizes[p])return "plane "+p;
          var luma=planes[0].getBuffer();
          for(int n=0;n<luma.remaining();++n){int value=luma.get(n)&0xff;low=Math.min(low,value);high=Math.max(high,value);}
          codec.releaseOutputBuffer(index,false);
          try{image.getPlanes();return "image valid after release";}catch(IllegalStateException expected){}
        } else codec.releaseOutputBuffer(index,false);
      }
      if(!eos || frames!=12)return "image EOS/frame count";
      // The Y plane is the decoder's own luma (NV12 output, as on Quest), so values
      // outside 16..235 in the stream reach it unchanged; require a real picture.
      if(high-low<32)return "luma range "+low+".."+high;
      codec.stop();codec.release();codec=null;
      return null;
    } catch(Throwable e){return "image codec: "+e;}
    finally {if(codec!=null)codec.release();}
  }
  // finds its video track only through MediaPlayer.getTrackInfo().
  public static String video(String fixture) {
    var extractor=new android.media.MediaExtractor();var player=new MediaPlayer();
    try (FileInputStream file=new FileInputStream(fixture)) {
      extractor.setDataSource(file.getFD(),0,file.getChannel().size());
      if(extractor.getTrackCount()!=1)return "extractor track count "+extractor.getTrackCount();
      var format=extractor.getTrackFormat(0);
      if(!"video/avc".equals(format.getString("mime")) || format.getInteger("width")!=128 || format.getInteger("height")!=72)return "extractor format "+format;
      if(format.getByteBuffer("csd-0")==null)return "extractor csd-0";
      extractor.selectTrack(0);
      var heap=java.nio.ByteBuffer.allocate(1<<20);var direct=java.nio.ByteBuffer.allocateDirect(1<<20);
      int samples=0;
      do {
        long time=extractor.getSampleTime();
        if(extractor.getSampleTrackIndex()!=0 || time<0)return "extractor sample track";
        int size=extractor.readSampleData(samples%2==0?heap:direct,4);
        var buffer=samples%2==0?heap:direct;
        if(size<=0 || buffer.position()!=4 || buffer.remaining()!=size || size!=extractor.getSampleSize())return "extractor sample size";
        samples++;
      } while(extractor.advance());
      if(samples!=12 || extractor.getSampleTime()!=-1 || extractor.readSampleData(heap,0)!=-1)return "extractor end "+samples;
      extractor.seekTo(0,android.media.MediaExtractor.SEEK_TO_PREVIOUS_SYNC);
      if(extractor.getSampleTime()!=0)return "extractor seek";
      player.setDataSource(file.getFD(),0,file.getChannel().size());player.prepare();
      var tracks=player.getTrackInfo();
      if(tracks.length!=1 || tracks[0].getTrackType()!=MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_VIDEO ||
         tracks[0].getFormat()!=null || !"und".equals(tracks[0].getLanguage()))return "track info";
      player.selectTrack(0);
      if(player.getSelectedTrack(MediaPlayer.TrackInfo.MEDIA_TRACK_TYPE_VIDEO)!=0)return "selected video track";
      return null;
    } catch(Throwable e){return "video tracks: "+e;}
    finally {extractor.release();player.release();}
  }
  // Callback arrival includes JVM/codec/worker startup; this only guards hangs.
  private static final int HANG_SECONDS = 120;
  public static String run(String fixture) {
    String callbackFailure=callbackMonitor();if(callbackFailure!=null)return callbackFailure;
    HandlerThread thread = new HandlerThread("media-flow-test"); thread.start();
    CountDownLatch done = new CountDownLatch(1);
    String[] failure = {null};
    new Handler(thread.getLooper()).post(() -> {
      MediaPlayer player = new MediaPlayer();
      try (FileInputStream file = new FileInputStream(fixture)) {
        file.getChannel().position(17);
        player.setDataSource(file.getFD(), 0, file.getChannel().size());
        player.prepare();
        if (player.getDuration() != 250) throw new AssertionError("duration");
        player.setLooping(true); player.start();
        Thread.sleep(120); player.pause();
        int paused = player.getCurrentPosition(); Thread.sleep(30);
        if (player.getCurrentPosition() != paused) throw new AssertionError("pause clock");
        player.seekTo(70); player.setLooping(false); player.start();
        player.stop(); player.prepare(); player.start();
        if (file.getChannel().position() != 17) throw new AssertionError("descriptor position");
        player.release(); player.release();
        boolean rejected = false;
        try { player.start(); } catch (IllegalStateException expected) { rejected = true; }
        if (!rejected) throw new AssertionError("released player");
      } catch (Throwable error) { failure[0] = "sync lifecycle: " + error.getClass().getSimpleName(); }
      finally { player.release(); done.countDown(); }
    });
    try {
      if (!done.await(HANG_SECONDS, TimeUnit.SECONDS)) return "sync lifecycle timeout";
      if (failure[0] != null) return failure[0];
      CountDownLatch completed = new CountDownLatch(1);
      AtomicInteger prepared = new AtomicInteger(), sought = new AtomicInteger(), ended = new AtomicInteger();
      new Handler(thread.getLooper()).post(() -> {
        MediaPlayer player = new MediaPlayer();
        try (FileInputStream file = new FileInputStream(fixture)) {
          player.setDataSource(file.getFD(), 0, file.getChannel().size());
          player.setOnErrorListener((p, what, extra) -> {
            failure[0] = "async media error"; p.release(); completed.countDown(); return true;
          });
          player.setOnPreparedListener(p -> {
            prepared.incrementAndGet(); p.seekTo(30); p.start();
          });
          player.setOnSeekCompleteListener(p -> sought.incrementAndGet());
          player.setOnCompletionListener(p -> { ended.incrementAndGet(); p.release(); completed.countDown(); });
          player.prepareAsync();
          // Closing the caller's descriptor cannot invalidate the owned media source.
        } catch (Throwable error) {
          failure[0] = "async setup: " + error.getClass().getSimpleName();
          player.release(); completed.countDown();
        }
      });
      if (!completed.await(HANG_SECONDS, TimeUnit.SECONDS)) return "async callback timeout";
      if (failure[0] != null) return failure[0];
      if (prepared.get() != 1 || sought.get() != 1 || ended.get() != 1) return "callback counts";
      CountDownLatch rejected = new CountDownLatch(1);
      AtomicInteger errors = new AtomicInteger(), fallback = new AtomicInteger();
      new Handler(thread.getLooper()).post(() -> {
        MediaPlayer player = new MediaPlayer();
        try (FileInputStream file = new FileInputStream(fixture)) {
          player.setDataSource(file.getFD(), 0, 8);
          player.setOnErrorListener((p, what, extra) -> { errors.incrementAndGet(); return false; });
          player.setOnCompletionListener(p -> { fallback.incrementAndGet(); p.release(); rejected.countDown(); });
          player.prepareAsync();
        } catch (Throwable error) { failure[0] = "invalid source setup"; player.release(); rejected.countDown(); }
      });
      if (!rejected.await(HANG_SECONDS, TimeUnit.SECONDS)) return "error callback timeout";
      if (errors.get() != 1 || fallback.get() != 1) return "error fallback counts";
      CountDownLatch cancelled = new CountDownLatch(1);
      AtomicInteger stale = new AtomicInteger();
      new Handler(thread.getLooper()).post(() -> {
        MediaPlayer player = new MediaPlayer();
        try (FileInputStream file = new FileInputStream(fixture)) {
          player.setDataSource(file.getFD(), 0, file.getChannel().size());
          player.setOnPreparedListener(p -> stale.incrementAndGet());
          player.setOnErrorListener((p, what, extra) -> { stale.incrementAndGet(); return true; });
          player.prepareAsync(); player.reset(); player.release();
          new Handler(thread.getLooper()).postDelayed(cancelled::countDown, 100);
        } catch (Throwable error) { failure[0] = "cancel setup"; player.release(); cancelled.countDown(); }
      });
      if (!cancelled.await(HANG_SECONDS, TimeUnit.SECONDS)) return "cancel timeout";
      if (stale.get() != 0) return "stale callback after reset/release";
      if (failure[0] != null) return failure[0];
      return null;
    } catch (InterruptedException error) { Thread.currentThread().interrupt(); return "interrupted"; }
    finally { thread.quitSafely(); try { thread.join(1000); } catch (InterruptedException ignored) { Thread.currentThread().interrupt(); } }
  }
}
