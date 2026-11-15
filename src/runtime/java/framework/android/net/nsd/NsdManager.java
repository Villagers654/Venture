package android.net.nsd;

import android.os.Handler;
import android.os.Looper;
import java.util.HashMap;
import java.util.Map;

/**
 * Network service discovery (DNS-SD over mDNS), API 32 surface.
 *
 * Venture has no mDNS responder for the app, so it answers as a device whose
 * NSD daemon cannot act: registering and resolving fail with
 * FAILURE_INTERNAL_ERROR, and discovery starts and finds nothing until it is
 * stopped. Arguments are checked as AOSP checks them, listeners are tracked
 * the same way (one request per listener), and every callback arrives on the
 * main looper as from AOSP's service handler.
 */
public final class NsdManager {
  public static final String ACTION_NSD_STATE_CHANGED = "android.net.nsd.STATE_CHANGED";
  public static final String EXTRA_NSD_STATE = "nsd_state";
  public static final int NSD_STATE_DISABLED = 1, NSD_STATE_ENABLED = 2;
  public static final int PROTOCOL_DNS_SD = 0x0001;
  public static final int FAILURE_INTERNAL_ERROR = 0, FAILURE_ALREADY_ACTIVE = 3, FAILURE_MAX_LIMIT = 4;

  public interface DiscoveryListener {
    void onStartDiscoveryFailed(String serviceType, int errorCode);
    void onStopDiscoveryFailed(String serviceType, int errorCode);
    void onDiscoveryStarted(String serviceType);
    void onDiscoveryStopped(String serviceType);
    void onServiceFound(NsdServiceInfo serviceInfo);
    void onServiceLost(NsdServiceInfo serviceInfo);
  }
  public interface RegistrationListener {
    void onRegistrationFailed(NsdServiceInfo serviceInfo, int errorCode);
    void onUnregistrationFailed(NsdServiceInfo serviceInfo, int errorCode);
    void onServiceRegistered(NsdServiceInfo serviceInfo);
    void onServiceUnregistered(NsdServiceInfo serviceInfo);
  }
  public interface ResolveListener {
    void onResolveFailed(NsdServiceInfo serviceInfo, int errorCode);
    void onServiceResolved(NsdServiceInfo serviceInfo);
  }

  private final Map<Object, String> active = new HashMap<>();  // listener -> discovered service type

  public NsdManager() {}

  // Callbacks run on the main looper; before it exists (embedded fixtures), on a daemon thread.
  private static void post(Runnable callback) {
    Looper main = Looper.getMainLooper();
    if (main != null) {
      new Handler(main).post(callback);
      return;
    }
    Thread thread = new Thread(callback, "NsdManager");
    thread.setDaemon(true);
    thread.start();
  }

  private synchronized void claim(Object listener, String detail) {
    if (listener == null) throw new IllegalArgumentException("listener cannot be null");
    if (active.containsKey(listener)) throw new IllegalArgumentException("listener already in use");
    active.put(listener, detail);
  }
  private synchronized String release(Object listener) {
    if (listener == null) throw new IllegalArgumentException("listener cannot be null");
    if (!active.containsKey(listener)) throw new IllegalArgumentException("listener not registered");
    return active.remove(listener);
  }
  private synchronized void forget(Object listener) { active.remove(listener); }
  private static void checkProtocol(int protocolType) {
    if (protocolType != PROTOCOL_DNS_SD) throw new IllegalArgumentException("Unsupported protocol");
  }
  private static void checkServiceInfo(NsdServiceInfo info) {
    if (info == null) throw new IllegalArgumentException("NsdServiceInfo cannot be null");
    if (info.getServiceName() == null || info.getServiceName().isEmpty())
      throw new IllegalArgumentException("Service name cannot be empty");
    if (info.getServiceType() == null || info.getServiceType().isEmpty())
      throw new IllegalArgumentException("Service type cannot be empty");
  }

  public void registerService(NsdServiceInfo serviceInfo, int protocolType, RegistrationListener listener) {
    checkServiceInfo(serviceInfo);
    if (serviceInfo.getPort() <= 0) throw new IllegalArgumentException("Invalid port number");
    checkProtocol(protocolType);
    claim(listener, null);
    post(() -> {
      forget(listener);
      listener.onRegistrationFailed(serviceInfo, FAILURE_INTERNAL_ERROR);
    });
  }
  public void unregisterService(RegistrationListener listener) {
    release(listener);  // a failed registration has already released its listener
  }
  public void discoverServices(String serviceType, int protocolType, DiscoveryListener listener) {
    if (serviceType == null || serviceType.isEmpty()) throw new IllegalArgumentException("Service type cannot be empty");
    checkProtocol(protocolType);
    claim(listener, serviceType);
    post(() -> listener.onDiscoveryStarted(serviceType));
  }
  public void stopServiceDiscovery(DiscoveryListener listener) {
    String serviceType = release(listener);
    post(() -> listener.onDiscoveryStopped(serviceType));
  }
  public void resolveService(NsdServiceInfo serviceInfo, ResolveListener listener) {
    checkServiceInfo(serviceInfo);
    claim(listener, null);
    post(() -> {
      forget(listener);
      listener.onResolveFailed(serviceInfo, FAILURE_INTERNAL_ERROR);
    });
  }
}
