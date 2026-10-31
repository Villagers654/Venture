package android.os;
// Android's developer diagnostics. Policies are kept and returned as set, as
// on a release build with no violations reported; nothing is detected or penalized.
public final class StrictMode {
  public static final class ThreadPolicy {
    public static final ThreadPolicy LAX = new ThreadPolicy(0);
    final int mask;
    ThreadPolicy(int mask) { this.mask = mask; }
    @Override public String toString() { return "[StrictMode.ThreadPolicy; mask=" + mask + "]"; }
    public static final class Builder {
      private int mask;
      public Builder() {}
      public Builder(ThreadPolicy policy) { mask = policy.mask; }
      private Builder set(int bits) { mask |= bits; return this; }
      private Builder clear(int bits) { mask &= ~bits; return this; }
      public Builder detectAll() { return set(0xff); }
      public Builder permitAll() { return clear(0xff); }
      public Builder detectCustomSlowCalls() { return set(1 << 0); }
      public Builder permitCustomSlowCalls() { return clear(1 << 0); }
      public Builder detectDiskReads() { return set(1 << 1); }
      public Builder permitDiskReads() { return clear(1 << 1); }
      public Builder detectDiskWrites() { return set(1 << 2); }
      public Builder permitDiskWrites() { return clear(1 << 2); }
      public Builder detectNetwork() { return set(1 << 3); }
      public Builder permitNetwork() { return clear(1 << 3); }
      public Builder detectResourceMismatches() { return set(1 << 4); }
      public Builder permitResourceMismatches() { return clear(1 << 4); }
      public Builder detectUnbufferedIo() { return set(1 << 5); }
      public Builder permitUnbufferedIo() { return clear(1 << 5); }
      public Builder detectExplicitGc() { return set(1 << 6); }
      public Builder permitExplicitGc() { return clear(1 << 6); }
      public Builder penaltyDeath() { return this; }
      public Builder penaltyDeathOnNetwork() { return this; }
      public Builder penaltyDialog() { return this; }
      public Builder penaltyDropBox() { return this; }
      public Builder penaltyFlashScreen() { return this; }
      public Builder penaltyLog() { return this; }
      public Builder penaltyListener(java.util.concurrent.Executor executor, OnThreadViolationListener listener) {
        return this;
      }
      public ThreadPolicy build() { return new ThreadPolicy(mask); }
    }
  }
  public static final class VmPolicy {
    public static final VmPolicy LAX = new VmPolicy(0);
    final int mask;
    VmPolicy(int mask) { this.mask = mask; }
    @Override public String toString() { return "[StrictMode.VmPolicy; mask=" + mask + "]"; }
    public static final class Builder {
      private int mask;
      public Builder() {}
      public Builder(VmPolicy policy) { mask = policy.mask; }
      private Builder set(int bits) { mask |= bits; return this; }
      public Builder detectAll() { return set(0xffff); }
      public Builder detectActivityLeaks() { return set(1 << 0); }
      public Builder detectCleartextNetwork() { return set(1 << 1); }
      public Builder detectContentUriWithoutPermission() { return set(1 << 2); }
      public Builder detectCredentialProtectedWhileLocked() { return set(1 << 3); }
      public Builder detectFileUriExposure() { return set(1 << 4); }
      public Builder detectImplicitDirectBoot() { return set(1 << 5); }
      public Builder detectIncorrectContextUse() { return set(1 << 6); }
      public Builder detectLeakedClosableObjects() { return set(1 << 7); }
      public Builder detectLeakedRegistrationObjects() { return set(1 << 8); }
      public Builder detectLeakedSqlLiteObjects() { return set(1 << 9); }
      public Builder detectNonSdkApiUsage() { return set(1 << 10); }
      public Builder detectUnsafeIntentLaunch() { return set(1 << 11); }
      public Builder detectUntaggedSockets() { return set(1 << 12); }
      public Builder permitNonSdkApiUsage() { mask &= ~(1 << 10); return this; }
      public Builder permitUnsafeIntentLaunch() { mask &= ~(1 << 11); return this; }
      public Builder setClassInstanceLimit(Class<?> klass, int instanceLimit) { return this; }
      public Builder penaltyDeath() { return this; }
      public Builder penaltyDeathOnCleartextNetwork() { return this; }
      public Builder penaltyDeathOnFileUriExposure() { return this; }
      public Builder penaltyDropBox() { return this; }
      public Builder penaltyLog() { return this; }
      public Builder penaltyListener(java.util.concurrent.Executor executor, OnVmViolationListener listener) {
        return this;
      }
      public VmPolicy build() { return new VmPolicy(mask); }
    }
  }
  public interface OnThreadViolationListener {
    void onThreadViolation(Throwable violation);
  }
  public interface OnVmViolationListener {
    void onVmViolation(Throwable violation);
  }
  private static final ThreadLocal<ThreadPolicy> threadPolicy = ThreadLocal.withInitial(() -> ThreadPolicy.LAX);
  private static volatile VmPolicy vmPolicy = VmPolicy.LAX;
  private StrictMode() {}
  public static void setThreadPolicy(ThreadPolicy policy) {
    threadPolicy.set(policy == null ? ThreadPolicy.LAX : policy);
  }
  public static ThreadPolicy getThreadPolicy() { return threadPolicy.get(); }
  public static ThreadPolicy allowThreadDiskReads() {
    ThreadPolicy old = getThreadPolicy();
    setThreadPolicy(new ThreadPolicy.Builder(old).permitDiskReads().build());
    return old;
  }
  public static ThreadPolicy allowThreadDiskWrites() {
    ThreadPolicy old = getThreadPolicy();
    setThreadPolicy(new ThreadPolicy.Builder(old).permitDiskWrites().permitDiskReads().build());
    return old;
  }
  public static void setVmPolicy(VmPolicy policy) { vmPolicy = policy == null ? VmPolicy.LAX : policy; }
  public static VmPolicy getVmPolicy() { return vmPolicy; }
  public static void enableDefaults() {}
  public static void noteSlowCall(String name) {}
}
