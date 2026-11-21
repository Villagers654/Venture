package android.content;

/** Venture's host context delivers broadcasts through BroadcastReceiver's package-private entry. */
public final class VentureReceivers {
  private VentureReceivers() {}

  public static void deliver(BroadcastReceiver receiver, Context context, Intent intent) {
    receiver.ventureDeliver(context, intent);
  }
}
