package horizonos.os;

/** Earlier Horizon OS name of the OS SDK version (see metavr.os.Build). */
public final class Build {
    private Build() {}

    public static final class HorizonOsSdk {
        private HorizonOsSdk() {}
        public static int getVersion() { return metavr.os.Build.SDK_VERSION; }
    }

    public static final class Version {
        private Version() {}
        public static int getApiVersion() { return metavr.os.Build.SDK_VERSION; }
        public static String getDisplayName() { return metavr.os.Build.DISPLAY_NAME; }
    }
}
