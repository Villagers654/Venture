public class BridgeFixture {
    public static class Child extends BridgeFixture {
        public Child() { super(0); }
        @Override public int integer() { return 777; }
    }
    public final int seed;
    public BridgeFixture(int value) { seed = value; }
    public Object obj() { return this; }
    public boolean bool() { return true; }
    public byte byt() { return -128; }
    public char chr() { return '\uffee'; }
    public short shrt() { return -32000; }
    public int integer() { return -1234567; }
    public long lng() { return -0x123456789abcdefL; }
    public float flt() { return 1.25f; }
    public double dbl() { return -2.125; }
    public void nothing() {}
    public static Object sobj() { return "fixture"; }
    public static Object echo(Object input) { return input; }
    public static boolean sbool() { return true; }
    public static byte sbyt() { return -128; }
    public static char schr() { return '\uffee'; }
    public static short sshrt() { return -32000; }
    public static int sinteger() { return -1234567; }
    public static long slng() { return -0x123456789abcdefL; }
    public static float sflt() { return 1.25f; }
    public static double sdbl() { return -2.125; }
    public static void snothing() {}
    public long mixed(long a,float b,long c,float d,long e,float f,long g,float h,long i,float j,long k,float l,long m,float n,long o,float p,long q,float r,long s,float t) {
        return a+c+e+g+i+k+m+o+q+s + (long)(b+d+f+h+j+l+n+p+r+t);
    }
    public static int fail() { throw new IllegalStateException("fixture exception"); }
    public static native boolean touch(long ptr, Object event, int a,int b,int c,int d,int e,long f,long g,int h,int i,int j,int k,int l,int m,float x,float y);
    public static native long reverse(long a,float b,long c,float d,long e,float f,long g,float h,long i,float j,long k,float l,long m,float n,long o,float p,long q,float r,long s,float t);
    public static native byte narrow();
    public static native String paths(String first, int i, long l, double d, String last);
    public static native char unicode();
    public static native short signedShort();
    public static native float fp();
    public static native double dp();
    public static double reflectedOnly(int i, float f) { return i + f; }
    public static boolean exportsInitialized = false;
    public static native Object reference(Object input);
    public static native void throwsGuest();
    public static native int recurse(int depth);
    public static int javaRecurse(int depth) { return depth == 0 ? 7 : recurse(depth - 1) + 1; }
    public static long exerciseReverse() { return reverse(1,1.25f,2,2.25f,3,3.25f,4,4.25f,5,5.25f,6,6.25f,7,7.25f,8,8.25f,9,9.25f,10,10.25f); }
    public static boolean exerciseTouch() { return touch(0x123456789abcdefL,new Object(),1,2,3,4,5,6,7,8,9,10,11,12,13,1.25f,2.5f); }

    private static void checkMixed(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) {
        if (i0 != 100L || f0 != 0.25f) throw new AssertionError("mixed parameter 0");
        if (i1 != 101L || f1 != 1.25f) throw new AssertionError("mixed parameter 1");
        if (i2 != 102L || f2 != 2.25f) throw new AssertionError("mixed parameter 2");
        if (i3 != 103L || f3 != 3.25f) throw new AssertionError("mixed parameter 3");
        if (i4 != 104L || f4 != 4.25f) throw new AssertionError("mixed parameter 4");
        if (i5 != 105L || f5 != 5.25f) throw new AssertionError("mixed parameter 5");
        if (i6 != 106L || f6 != 6.25f) throw new AssertionError("mixed parameter 6");
        if (i7 != 107L || f7 != 7.25f) throw new AssertionError("mixed parameter 7");
        if (i8 != 108L || f8 != 8.25f) throw new AssertionError("mixed parameter 8");
        if (i9 != 109L || f9 != 9.25f) throw new AssertionError("mixed parameter 9");
    }
    public Object mixedobj(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) { checkMixed(i0, f0, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7, f7, i8, f8, i9, f9); return this; }
    public static Object smixedobj(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) { checkMixed(i0, f0, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7, f7, i8, f8, i9, f9); return "fixture"; }
    public boolean mixedbool(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) { checkMixed(i0, f0, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7, f7, i8, f8, i9, f9); return true; }
    public static boolean smixedbool(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) { checkMixed(i0, f0, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7, f7, i8, f8, i9, f9); return true; }
    public byte mixedbyt(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) { checkMixed(i0, f0, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7, f7, i8, f8, i9, f9); return (byte)-128; }
    public static byte smixedbyt(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) { checkMixed(i0, f0, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7, f7, i8, f8, i9, f9); return (byte)-128; }
    public char mixedchr(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) { checkMixed(i0, f0, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7, f7, i8, f8, i9, f9); return '\uffee'; }
    public static char smixedchr(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) { checkMixed(i0, f0, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7, f7, i8, f8, i9, f9); return '\uffee'; }
    public short mixedshrt(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) { checkMixed(i0, f0, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7, f7, i8, f8, i9, f9); return (short)-32000; }
    public static short smixedshrt(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) { checkMixed(i0, f0, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7, f7, i8, f8, i9, f9); return (short)-32000; }
    public int mixedinteger(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) { checkMixed(i0, f0, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7, f7, i8, f8, i9, f9); return -1234567; }
    public static int smixedinteger(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) { checkMixed(i0, f0, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7, f7, i8, f8, i9, f9); return -1234567; }
    public long mixedlng(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) { checkMixed(i0, f0, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7, f7, i8, f8, i9, f9); return -0x123456789abcdefL; }
    public static long smixedlng(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) { checkMixed(i0, f0, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7, f7, i8, f8, i9, f9); return -0x123456789abcdefL; }
    public float mixedflt(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) { checkMixed(i0, f0, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7, f7, i8, f8, i9, f9); return 1.25f; }
    public static float smixedflt(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) { checkMixed(i0, f0, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7, f7, i8, f8, i9, f9); return 1.25f; }
    public double mixeddbl(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) { checkMixed(i0, f0, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7, f7, i8, f8, i9, f9); return -2.125; }
    public static double smixeddbl(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) { checkMixed(i0, f0, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7, f7, i8, f8, i9, f9); return -2.125; }
    public void mixednothing(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) { checkMixed(i0, f0, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7, f7, i8, f8, i9, f9);  }
    public static void smixednothing(long i0, float f0, long i1, float f1, long i2, float f2, long i3, float f3, long i4, float f4, long i5, float f5, long i6, float f6, long i7, float f7, long i8, float f8, long i9, float f9) { checkMixed(i0, f0, i1, f1, i2, f2, i3, f3, i4, f4, i5, f5, i6, f6, i7, f7, i8, f8, i9, f9);  }
}
