package metavr.os;

/**
 * Meta VR OS SDK version (JSDK metavr.os.Build). Horizon OS supplies this class at run time;
 * games that check the OS version read it, directly or through Meta's utility library
 * (metavrx.os.MetaVrOsSdkVersion), and without it a game took the headset for one that
 * needs a system update (Starworn). The older names, horizonos.os.Build and
 * vros.os.VrosBuild, report the same number.
 */
public final class Build {
    /** The newest OS SDK this framework knows; the floor of what it reports. */
    public static final int KNOWN_SDK_VERSION = 207;
    /**
     * The Meta VR OS SDK version reported: the newest the launcher found published
     * (VENTURE_METAVR_OS_SDK, from Maven's listing of Meta's VR OS Java SDK), or the
     * newest this framework knows, whichever is higher.
     */
    public static final int SDK_VERSION = sdkVersion(System.getenv("VENTURE_METAVR_OS_SDK"));
    /** Opaque user-facing OS label; apps are told not to parse it. */
    public static final String DISPLAY_NAME = "v" + SDK_VERSION;

    private Build() {}

    static int sdkVersion(String published) {
        try {
            return published == null ? KNOWN_SDK_VERSION : Math.max(KNOWN_SDK_VERSION, Integer.parseInt(published.trim()));
        } catch (NumberFormatException e) {
            return KNOWN_SDK_VERSION;
        }
    }

    public static final class Version {
        private Version() {}
        public static int getApiVersion() { return SDK_VERSION; }
        public static String getDisplayName() { return DISPLAY_NAME; }
    }
}
