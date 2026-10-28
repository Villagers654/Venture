package android.app;
import android.content.Context;
import android.os.Bundle;
import android.os.Handler;
import android.view.View;
import android.view.ViewGroup;
import android.view.LayoutInflater;

/** Headless framework fragments, including the legacy Unity permission helper. */
public class Fragment {
  Activity activity;
  FragmentManager manager;
  String tag;
  int stage;
  private Bundle arguments;
  private boolean retainInstance;
  public Activity getActivity() { return activity; }
  public Context getContext() { return activity; }
  public FragmentManager getFragmentManager() { return manager; }
  public String getTag() { return tag; }
  public boolean isAdded() { return manager != null; }
  public boolean isResumed() { return stage == 3; }
  // AOSP core/java/android/app/Fragment.java: this is a configuration-change
  // policy, not a lifetime extension after explicit removal or Activity finish.
  // Our hosted activities handle configuration changes without re-creation.
  public void setRetainInstance(boolean retain) { retainInstance = retain; }
  public boolean getRetainInstance() { return retainInstance; }
  public void setArguments(Bundle value) {
    if (manager != null) throw new IllegalStateException("Fragment already added");
    arguments = value;
  }
  public Bundle getArguments() { return arguments; }
  public void onAttach(Context context) { if (context instanceof Activity a) onAttach(a); }
  public void onAttach(Activity activity) {}
  public void onCreate(Bundle state) {}
  public void onActivityCreated(Bundle state) {}
  public View onCreateView(LayoutInflater inflater, ViewGroup container, Bundle state) { return null; }
  public void onStart() {}
  public void onResume() {}
  public void onPause() {}
  public void onStop() {}
  public void onDestroy() {}
  public void onDetach() {}
  public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {}
  public void requestPermissions(String[] permissions, int request) {
    if (activity == null) throw new IllegalStateException("Fragment not attached");
    String[] names = permissions.clone();
    int[] results = java.util.Arrays.stream(names).mapToInt(activity::checkSelfPermission).toArray();
    new Handler(activity.getMainLooper()).post(() -> {
      if (manager != null) onRequestPermissionsResult(request, names, results);
    });
  }
  public boolean shouldShowRequestPermissionRationale(String permission) {
    if (activity == null) throw new IllegalStateException("Fragment not attached");
    return activity.shouldShowRequestPermissionRationale(permission);
  }
}
