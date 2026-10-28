package android.net;

/** Passive network observation filter; this never creates or binds a connection. */
public final class NetworkRequest {
  private final long capabilities;
  private final int transports;
  private NetworkRequest(long capabilities, int transports) {
    this.capabilities = capabilities;
    this.transports = transports;
  }
  boolean matches(NetworkCapabilities network) {
    for (int capability = 0; capability <= 32; capability++)
      if ((capabilities & (1L << capability)) != 0 && !network.hasCapability(capability)) return false;
    if (transports == 0) return true;
    for (int transport = 0; transport <= 8; transport++)
      if ((transports & (1 << transport)) != 0 && network.hasTransport(transport)) return true;
    return false;
  }
  public static final class Builder {
    // AOSP Android 12L Connectivity: NetworkCapabilities.DEFAULT_CAPABILITIES.
    // Capabilities are conjunctive; transports are alternatives. build() copies.
    private long capabilities = (1L << 13) | (1L << 14) | (1L << 15);
    private int transports;
    public Builder() {}
    public Builder addCapability(int capability) {
      if (capability < 0 || capability > 32) throw new IllegalArgumentException("Invalid capability");
      capabilities |= 1L << capability;
      return this;
    }
    public Builder addTransportType(int transport) {
      if (transport < 0 || transport > 8) throw new IllegalArgumentException("Invalid transport");
      transports |= 1 << transport;
      return this;
    }
    public NetworkRequest build() { return new NetworkRequest(capabilities, transports); }
  }
}
