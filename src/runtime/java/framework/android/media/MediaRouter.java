package android.media;

import android.content.Context;
import android.view.Display;
import java.util.IdentityHashMap;

/** The in-process device route; no remote casting/presentation provider is installed. */
public class MediaRouter {
  public static final int ROUTE_TYPE_LIVE_AUDIO = 1, ROUTE_TYPE_LIVE_VIDEO = 2,
      ROUTE_TYPE_USER = 0x800000, CALLBACK_FLAG_PERFORM_ACTIVE_SCAN = 1,
      CALLBACK_FLAG_UNFILTERED_EVENTS = 2, CALLBACK_FLAG_REQUEST_DISCOVERY = 4,
      CALLBACK_FLAG_PASSIVE_DISCOVERY = 8;
  private final RouteInfo local = new RouteInfo();
  private final IdentityHashMap<Callback, Integer> callbacks = new IdentityHashMap<>();
  public RouteInfo getDefaultRoute() { return local; }
  public RouteInfo getSelectedRoute(int types) { return local; }
  public int getRouteCount() { return 1; }
  public RouteInfo getRouteAt(int index) {
    if (index != 0) throw new IndexOutOfBoundsException(index);
    return local;
  }
  public synchronized void addCallback(int types, Callback callback) { addCallback(types, callback, 0); }
  public synchronized void addCallback(int types, Callback callback, int flags) {
    java.util.Objects.requireNonNull(callback);
    callbacks.merge(callback, types, (a, b) -> a | b);
  }
  public synchronized void removeCallback(Callback callback) { callbacks.remove(callback); }
  public void selectRoute(int types, RouteInfo route) {
    if (route != local) throw new IllegalArgumentException("Route does not belong to this router");
    // The only route is already selected; Android does not emit a change here.
  }
  public static class RouteInfo {
    public static final int PLAYBACK_TYPE_LOCAL = 0, PLAYBACK_TYPE_REMOTE = 1,
        PLAYBACK_VOLUME_FIXED = 0, PLAYBACK_VOLUME_VARIABLE = 1;
    protected RouteInfo() {}
    public CharSequence getName() { return "Device"; }
    public CharSequence getName(Context context) { return getName(); }
    public CharSequence getDescription() { return "Local application output"; }
    public int getSupportedTypes() { return ROUTE_TYPE_LIVE_AUDIO | ROUTE_TYPE_LIVE_VIDEO; }
    public boolean matchesTypes(int types) { return (getSupportedTypes() & types) != 0; }
    public boolean isEnabled() { return true; }
    public int getPlaybackType() { return PLAYBACK_TYPE_LOCAL; }
    public int getPlaybackStream() { return AudioManager.STREAM_MUSIC; }
    // Android's presentation display is an additional display, never the default.
    public Display getPresentationDisplay() { return null; }
  }
  public static class RouteGroup extends RouteInfo {}
  public abstract static class Callback {
    public abstract void onRouteSelected(MediaRouter router, int type, RouteInfo info);
    public abstract void onRouteUnselected(MediaRouter router, int type, RouteInfo info);
    public abstract void onRouteAdded(MediaRouter router, RouteInfo info);
    public abstract void onRouteRemoved(MediaRouter router, RouteInfo info);
    public abstract void onRouteChanged(MediaRouter router, RouteInfo info);
    public abstract void onRouteGrouped(MediaRouter router, RouteInfo info, RouteGroup group, int index);
    public abstract void onRouteUngrouped(MediaRouter router, RouteInfo info, RouteGroup group);
    public abstract void onRouteVolumeChanged(MediaRouter router, RouteInfo info);
    public void onRoutePresentationDisplayChanged(MediaRouter router, RouteInfo info) {}
  }
  public static class SimpleCallback extends Callback {
    public void onRouteSelected(MediaRouter router, int type, RouteInfo info) {}
    public void onRouteUnselected(MediaRouter router, int type, RouteInfo info) {}
    public void onRouteAdded(MediaRouter router, RouteInfo info) {}
    public void onRouteRemoved(MediaRouter router, RouteInfo info) {}
    public void onRouteChanged(MediaRouter router, RouteInfo info) {}
    public void onRouteGrouped(MediaRouter router, RouteInfo info, RouteGroup group, int index) {}
    public void onRouteUngrouped(MediaRouter router, RouteInfo info, RouteGroup group) {}
    public void onRouteVolumeChanged(MediaRouter router, RouteInfo info) {}
  }
}
