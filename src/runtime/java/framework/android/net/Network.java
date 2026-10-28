package android.net;

/** Opaque snapshot of the host's active network; socket routing remains with the host OS. */
public final class Network {
  final int type;
  Network(int type) { this.type = type; }
}
