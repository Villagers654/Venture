package vros.os;

/** Earliest name of the OS SDK version (see metavr.os.Build). */
public final class VrosBuild {
    private VrosBuild() {}
    public static int getSdkVersion() { return metavr.os.Build.SDK_VERSION; }
}
