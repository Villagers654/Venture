package com.venture.runtime;

import java.lang.ref.Cleaner;
import java.nio.ByteBuffer;
import java.nio.ReadOnlyBufferException;

/**
 * Android direct ByteBuffer behavior for application code (the APK rewriter redirects
 * ByteBuffer.allocateDirect, array and arrayOffset calls here).
 *
 * On a device a direct buffer's storage is ordinary process memory, so native code may use it
 * through GetDirectBufferAddress. HotSpot allocates it from host memory, which a 32-bit ARM
 * guest cannot address; there the storage comes from the guest heap instead (Unity 2018's
 * UnityWebRequest upload buffer, for example). Other processes keep HotSpot's allocation.
 *
 * ART also backs a direct buffer with a non-movable byte[] that array() returns, and Unity's
 * UnityWebRequest writes array() to the request stream after native code filled the buffer.
 * HotSpot's direct buffers have no array, so array() here returns a copy of the buffer's
 * contents (offset 0): reads made through it see what native code wrote; writes made into it
 * do not reach the buffer.
 */
public final class DirectBuffers {
    private DirectBuffers() {}
    private static volatile boolean bound;
    private static final class Release {
        static final Cleaner CLEANER = Cleaner.create();
    }
    public static ByteBuffer allocateDirect(int capacity) {
        if (capacity < 0) throw new IllegalArgumentException("capacity < 0: (" + capacity + " < 0)");
        long address = bound ? allocate0(capacity) : 0;
        if (address == 0) return ByteBuffer.allocateDirect(capacity);
        ByteBuffer buffer;
        try {
            buffer = wrap0(address, capacity);
        } catch (RuntimeException | Error failure) {
            free0(address);
            throw failure;
        }
        // Slices and duplicates keep this buffer reachable, so storage lives as long as any view.
        Release.CLEANER.register(buffer, () -> free0(address));
        return buffer;
    }
    public static byte[] array(ByteBuffer buffer) {
        if (!buffer.isDirect() || buffer.hasArray()) return buffer.array();
        if (buffer.isReadOnly()) throw new ReadOnlyBufferException();
        byte[] contents = new byte[buffer.capacity()];
        buffer.get(0, contents);
        return contents;
    }
    public static int arrayOffset(ByteBuffer buffer) {
        if (!buffer.isDirect() || buffer.hasArray()) return buffer.arrayOffset();
        if (buffer.isReadOnly()) throw new ReadOnlyBufferException();
        return 0;
    }
    /** Zeroed guest storage for capacity bytes; 0 when HotSpot's own allocation is addressable. */
    private static native long allocate0(int capacity);
    private static native ByteBuffer wrap0(long address, int capacity);
    private static native void free0(long address);
}
