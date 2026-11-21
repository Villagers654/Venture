package com.venture.runtime.layoutlib;

import android.app.VentureActivities;
import android.view.IWindowManager;
import android.view.WindowManagerGlobal;
import android.view.inputmethod.InputMethodManager;
import com.android.internal.view.IInputMethodManager;
import java.lang.reflect.Field;

/**
 * The system services a 2D process's framework code reaches over Binder, served
 * in process (android.os.ServiceManager). Activity manager calls are served by
 * android.app.VentureActivities.
 */
final class SystemServices {
  private SystemServices() {}

  static void install() {
    windowManager();
    inputMethodManager();
    inputManager();
  }

  /**
   * The input manager service: the input devices layoutlib's natives describe
   * (Layoutlib.inputDevices), which never change. KeyCharacterMap.load asks it
   * for a device's key map, as an action bar's menu does when a window prepares
   * its panel.
   */
  private static void inputManager() {
    android.hardware.input.IInputManager service = VentureActivities.proxy(android.hardware.input.IInputManager.class,
        (method, args) -> {
          android.view.InputDevice[] devices = inputDevices();
          switch (method.getName()) {
            case "getInputDeviceIds": {
              int[] ids = new int[devices.length];
              for (int i = 0; i < ids.length; ++i) ids[i] = devices[i].getId();
              return ids;
            }
            case "getInputDevice":
              for (android.view.InputDevice device : devices)
                if (device.getId() == (int) args[0]) return device;
              return null;
            default:
              return null;
          }
        });
    android.hardware.input.InputManagerGlobal.sInstance = new android.hardware.input.InputManagerGlobal(service);
  }

  private static android.view.InputDevice[] devices;

  /**
   * layoutlib's devices, and Android's virtual keyboard (ID -1) when they lack
   * it: every Android device has one, and KeyCharacterMap.load falls back to it.
   * It takes the key map of layoutlib's keyboard (Generic.kcm).
   */
  private static synchronized android.view.InputDevice[] inputDevices() {
    if (devices != null) return devices;
    android.view.InputDevice[] known = Layoutlib.inputDevices();
    if (known == null) known = new android.view.InputDevice[0];
    android.view.KeyCharacterMap map = null;
    for (android.view.InputDevice device : known) {
      if (device.getId() == android.view.KeyCharacterMap.VIRTUAL_KEYBOARD) return devices = known;
      if (map == null) map = device.getKeyCharacterMap();
    }
    android.util.Log.i("InputManager", known.length + " input devices; virtual keyboard key map "
        + (map != null ? "from " + known[0].getName() : "missing"));
    if (map == null) return devices = known;
    android.view.InputDevice virtual = new android.view.InputDevice.Builder()
        .setId(android.view.KeyCharacterMap.VIRTUAL_KEYBOARD).setName("Virtual").setDescriptor("virtual")
        .setIsVirtualDevice(true).setSources(android.view.InputDevice.SOURCE_KEYBOARD)
        .setKeyboardType(android.view.InputDevice.KEYBOARD_TYPE_ALPHABETIC).setKeyCharacterMap(map).build();
    devices = java.util.Arrays.copyOf(known, known.length + 1);
    devices[known.length] = virtual;
    return devices;
  }

  /**
   * The input method manager service. A view's text input reaches Venture's
   * system keyboard in process (see InputMethodManager's Venture rules), so the
   * service only accepts the framework's client and focus reports.
   */
  private static void inputMethodManager() {
    IInputMethodManager service = VentureActivities.proxy(IInputMethodManager.class, (method, args) -> null);
    setStatic(InputMethodManager.class.getPackageName() + ".IInputMethodManagerGlobalInvoker", "sServiceCache", service);
  }

  private static void setStatic(String className, String name, Object value) {
    try {
      Field field = Class.forName(className).getDeclaredField(name);
      field.setAccessible(true);
      field.set(null, value);
    } catch (ReflectiveOperationException e) {
      throw new IllegalStateException(className + " has no field " + name, e);
    }
  }

  /**
   * The window manager service, as framework code sees it: windows are Venture's
   * panels, so it answers only the questions views ask (a device with a
   * navigation bar and no menu key, so action bars show their overflow menu, and
   * normal animation speed). WindowManagerGlobal caches it; set before any view
   * exists, its first lookup never reads the animator scale from system_server's
   * shared memory, which a 2D process does not have.
   */
  private static void windowManager() {
    IWindowManager service = VentureActivities.proxy(IWindowManager.class, (method, args) -> switch (method.getName()) {
      case "hasNavigationBar" -> true;
      case "getCurrentAnimatorScale" -> 1f;
      case "getAnimationScale" -> 1f;
      case "getAnimationScales" -> new float[] {1f, 1f, 1f};
      default -> null;
    });
    setStatic(WindowManagerGlobal.class.getName(), "sWindowManagerService", service);
  }
}
