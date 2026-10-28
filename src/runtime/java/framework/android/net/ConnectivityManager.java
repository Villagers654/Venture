package android.net;
import java.util.Objects;
import java.util.concurrent.*;
public class ConnectivityManager {
  public static final int TYPE_MOBILE = 0, TYPE_WIFI = 1, TYPE_ETHERNET = 9, TYPE_VPN = 17;
  private static final ScheduledExecutorService observer = Executors.newSingleThreadScheduledExecutor(task -> {
    Thread thread = new Thread(task, "VentureNetwork");
    thread.setDaemon(true);
    return thread;
  });
  private final ConcurrentHashMap<NetworkCallback, Registration> callbacks = new ConcurrentHashMap<>();
  private Network active;
  public synchronized Network getActiveNetwork() {
    int type = com.venture.runtime.RuntimeEnvironment.host().activeNetworkType();
    if (type == -2) active = null;
    else if (active == null || active.type != type) active = new Network(type);
    return active;
  }
  // AOSP lists every network the system tracks (deprecated in API 31); the
  // host snapshot exposes only its active route, so that is the inventory.
  public Network[] getAllNetworks() {
    Network network = getActiveNetwork();
    return network == null ? new Network[0] : new Network[] {network};
  }
  public NetworkCapabilities getNetworkCapabilities(Network network) {
    if (network == null || network != getActiveNetwork()) return null;
    return capabilitiesOf(network);
  }
  private NetworkCapabilities capabilitiesOf(Network network) {
    return new NetworkCapabilities(network.type, com.venture.runtime.RuntimeEnvironment.host().hasInternetConnectivity());
  }
  // Android NetworkCallback's base observer methods intentionally do nothing.
  public static class NetworkCallback {
    public void onAvailable(Network network) {}
    public void onCapabilitiesChanged(Network network, NetworkCapabilities capabilities) {}
    public void onLost(Network network) {}
    public void onUnavailable() {}
  }
  private final class Registration implements Runnable {
    final NetworkCallback callback;
    final NetworkRequest request;
    volatile boolean closed;
    ScheduledFuture<?> future;
    Network previous;
    NetworkCapabilities previousCapabilities;
    Registration(NetworkCallback callback, NetworkRequest request) {
      this.callback = callback;
      this.request = request;
    }
    public void run() {
      if (closed) return;
      try {
        Network current = getActiveNetwork();
        NetworkCapabilities capabilities = current == null ? null : capabilitiesOf(current);
        if (capabilities == null || request != null && !request.matches(capabilities))
          current = null;
        if (current == previous) {
          if (current != null && !capabilities.equals(previousCapabilities) && !closed) {
            previousCapabilities = capabilities;
            callback.onCapabilitiesChanged(current, capabilities);
          }
          return;
        }
        if (previous != null && !closed) callback.onLost(previous);
        previous = current;
        previousCapabilities = capabilities;
        if (current != null && !closed) {
          callback.onAvailable(current);
          if (!closed) callback.onCapabilitiesChanged(current, capabilities);
        }
      } catch (RuntimeException e) {
        android.util.Log.e("ConnectivityManager", "Host network observation failed");
      }
    }
  }
  public synchronized void registerDefaultNetworkCallback(NetworkCallback callback) {
    registerCallback(callback, null);
  }
  public synchronized void registerNetworkCallback(NetworkRequest request, NetworkCallback callback) {
    registerCallback(callback, Objects.requireNonNull(request));
  }
  private void registerCallback(NetworkCallback callback, NetworkRequest request) {
    Objects.requireNonNull(callback);
    Registration registration = new Registration(callback, request);
    if (callbacks.putIfAbsent(callback, registration) != null)
      throw new IllegalArgumentException("Network callback already registered");
    registration.future = observer.scheduleWithFixedDelay(registration, 0, 1, TimeUnit.SECONDS);
  }
  public synchronized void unregisterNetworkCallback(NetworkCallback callback) {
    Registration registration = callbacks.remove(Objects.requireNonNull(callback));
    if (registration == null) throw new IllegalArgumentException("Network callback not registered");
    registration.closed = true;
    registration.future.cancel(false);
  }
  public NetworkInfo getActiveNetworkInfo() {
    Network network = getActiveNetwork();
    return network == null ? null : new NetworkInfo(network.type, true);
  }
  public NetworkInfo getNetworkInfo(int type) {
    NetworkInfo active = getActiveNetworkInfo();
    if (active != null && active.getType() == type) return active;
    return switch (type) {
      case TYPE_MOBILE, TYPE_WIFI, TYPE_ETHERNET, TYPE_VPN -> new NetworkInfo(type, false);
      default -> null;
    };
  }
  public boolean isActiveNetworkMetered() {
    var network = getActiveNetwork();
    var capabilities = network == null ? null : capabilitiesOf(network);
    return capabilities == null || !capabilities.hasCapability(NetworkCapabilities.NET_CAPABILITY_NOT_METERED);
  }
}
