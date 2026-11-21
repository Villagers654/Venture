package android.provider;

import android.content.ContentResolver;
import android.database.ContentObserver;
import java.util.HashMap;
import java.util.Map;
import java.util.concurrent.Executor;

/**
 * DeviceConfig's store in a 2D process. On Android it is the settings provider's
 * "config" table, which the server fills from experiments; a desktop has no
 * server and no settings provider, so the store starts empty and every flag
 * reads its default, as on a device that never synced. Values an app sets
 * last for the process.
 */
public class SettingsConfigDataStore implements DeviceConfigDataStore {
  private static final Map<String, String> values = new HashMap<>();
  private static int syncDisabledMode;

  private static String key(String namespace, String name) { return namespace + "/" + name; }

  @Override
  public Map<String, String> getAllProperties() {
    synchronized (values) { return new HashMap<>(values); }
  }

  @Override
  public DeviceConfig.Properties getProperties(String namespace, String... names) {
    Map<String, String> found = new HashMap<>();
    synchronized (values) {
      String prefix = namespace + "/";
      for (Map.Entry<String, String> entry : values.entrySet()) {
        if (!entry.getKey().startsWith(prefix)) continue;
        String name = entry.getKey().substring(prefix.length());
        if (names.length == 0 || java.util.Arrays.asList(names).contains(name)) found.put(name, entry.getValue());
      }
    }
    return new DeviceConfig.Properties(namespace, found);
  }

  @Override
  public boolean setProperties(DeviceConfig.Properties properties) {
    synchronized (values) {
      String prefix = properties.getNamespace() + "/";
      values.keySet().removeIf(name -> name.startsWith(prefix));
      for (String name : properties.getKeyset()) values.put(key(properties.getNamespace(), name), properties.getString(name, null));
    }
    return true;
  }

  @Override
  public boolean setProperty(String namespace, String name, String value, boolean makeDefault) {
    synchronized (values) {
      if (value == null) values.remove(key(namespace, name));
      else values.put(key(namespace, name), value);
    }
    return true;
  }

  @Override
  public boolean deleteProperty(String namespace, String name) {
    synchronized (values) { return values.remove(key(namespace, name)) != null; }
  }

  @Override
  public void resetToDefaults(int resetMode, String namespace) {
    synchronized (values) {
      if (namespace == null) values.clear();
      else values.keySet().removeIf(name -> name.startsWith(namespace + "/"));
    }
  }

  @Override
  public void setSyncDisabledMode(int mode) { syncDisabledMode = mode; }

  @Override
  public int getSyncDisabledMode() { return syncDisabledMode; }

  @Override
  public void setMonitorCallback(ContentResolver resolver, Executor executor, DeviceConfig.MonitorCallback callback) {}

  @Override
  public void clearMonitorCallback(ContentResolver resolver) {}

  @Override
  public void registerContentObserver(String namespace, boolean descendants, ContentObserver observer) {}

  @Override
  public void unregisterContentObserver(ContentObserver observer) {}
}
