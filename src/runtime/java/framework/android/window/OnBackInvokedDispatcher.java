package android.window;
/** API 33 back dispatch. The back key reaches activities through onBackPressed here. */
public interface OnBackInvokedDispatcher {
  int PRIORITY_DEFAULT = 0, PRIORITY_OVERLAY = 1000000;
  void registerOnBackInvokedCallback(int priority, OnBackInvokedCallback callback);
  void unregisterOnBackInvokedCallback(OnBackInvokedCallback callback);
}
