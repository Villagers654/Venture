package android.media;

import java.io.Closeable;
import java.io.IOException;

// AOSP media/java/android/media/MediaDataSource.java (API 23): apps implement
// random-access reads and ownership. This contract does not imply a decoder.
public abstract class MediaDataSource implements Closeable {
  public MediaDataSource() {}
  public abstract int readAt(long position, byte[] buffer, int offset, int size) throws IOException;
  public abstract long getSize() throws IOException;
}
