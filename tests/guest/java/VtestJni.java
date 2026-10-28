package org.venture.tests;

// Compile/package later with the Java bridge; no Android SDK is needed to build
// the native fixture. System.loadLibrary must invoke JNI_OnLoad before vtest_main.
public final class VtestJni {
    static { System.loadLibrary("vtest_jni"); }
    public static native int registeredAdd(int a, int b);
    public static native int nativeMultiply(int a, int b);
    public static native String nativeGreeting();
    public static native int runGuest();
    public static void main(String[] args) {
        if (registeredAdd(19, 23) != 42 || nativeMultiply(6, 7) != 42
                || !"Venture JNI".equals(nativeGreeting()) || runGuest() != 0) {
            throw new AssertionError("Venture JNI fixture failed");
        }
    }
}
