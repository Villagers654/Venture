package android.view;
public class ContextThemeWrapper extends android.content.ContextWrapper {
  private int themeId;
  public ContextThemeWrapper() {
    super(null);
  }
  public ContextThemeWrapper(android.content.Context c, int theme) {
    super(c);
    themeId = theme;
    if (theme != 0)
      setTheme(theme);
  }
  public int getThemeResId() {
    return themeId;
  }
  public void setTheme(int theme) {
    themeId = theme;
    super.setTheme(theme);
  }
  public void applyOverrideConfiguration(android.content.res.Configuration config) {
    getResources().updateConfiguration(config, null);
  }
}
