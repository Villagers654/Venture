package android.view;
public interface WindowInsetsController {
  void hide(int types);
  void show(int types);
  void setSystemBarsBehavior(int behavior);
  int getSystemBarsBehavior();
  void setSystemBarsAppearance(int appearance, int mask);
  int getSystemBarsAppearance();
}
