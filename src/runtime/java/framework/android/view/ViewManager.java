package android.view;
public interface ViewManager {
  void addView(View view, ViewGroup.LayoutParams params);
  void removeView(View view);
  void updateViewLayout(View view, ViewGroup.LayoutParams params);
}
