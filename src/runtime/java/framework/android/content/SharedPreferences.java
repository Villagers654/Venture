package android.content;
import java.util.*;
public interface SharedPreferences {
  Map<String, ?> getAll();
  String getString(String key, String def);
  Set<String> getStringSet(String key, Set<String> def);
  int getInt(String key, int def);
  long getLong(String key, long def);
  float getFloat(String key, float def);
  boolean getBoolean(String key, boolean def);
  boolean contains(String key);
  Editor edit();
  void registerOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener listener);
  void unregisterOnSharedPreferenceChangeListener(OnSharedPreferenceChangeListener listener);
  interface OnSharedPreferenceChangeListener {
    void onSharedPreferenceChanged(SharedPreferences preferences, String key);
  }
  interface Editor {
    Editor putString(String key, String value);
    Editor putStringSet(String key, Set<String> value);
    Editor putInt(String key, int value);
    Editor putLong(String key, long value);
    Editor putFloat(String key, float value);
    Editor putBoolean(String key, boolean value);
    Editor remove(String key);
    Editor clear();
    boolean commit();
    void apply();
  }
}
