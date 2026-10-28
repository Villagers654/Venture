package android.view;
final class QuestWindowInsetsController implements WindowInsetsController {
  private int hidden, behavior, appearance;
  public void hide(int types) {
    hidden |= types;
  }
  public void show(int types) {
    hidden &= ~types;
  }
  public void setSystemBarsBehavior(int b) {
    behavior = b;
  }
  public int getSystemBarsBehavior() {
    return behavior;
  }
  public void setSystemBarsAppearance(int a, int mask) {
    appearance = (appearance & ~mask) | (a & mask);
  }
  public int getSystemBarsAppearance() {
    return appearance;
  }
}
