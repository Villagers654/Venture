public final class ExportFixture {
    static { BridgeFixture.exportsInitialized = true; }
    public static native int value(int input);
    public static native long overloaded(int input);
    public static native long overloaded(long input);
}
