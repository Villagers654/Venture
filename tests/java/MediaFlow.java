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
