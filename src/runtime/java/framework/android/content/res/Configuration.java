package android.content.res;
import java.util.Locale;
public class Configuration {
  public static final int ORIENTATION_UNDEFINED = 0, ORIENTATION_PORTRAIT = 1,
                          ORIENTATION_LANDSCAPE = 2, UI_MODE_NIGHT_NO = 0x10,
                          UI_MODE_NIGHT_YES = 0x20, UI_MODE_NIGHT_MASK = 0x30;
  public float fontScale = 1;
  // SDR, non-wide-gamut display (AOSP Configuration COLOR_MODE_* fields).
  public int colorMode = 0x5;
  public int fontWeightAdjustment = 0;
  public int mcc, mnc, orientation = 2, uiMode = 0x11, screenLayout = 0x22, touchscreen = 1,
                       keyboard = 1, keyboardHidden = 2, hardKeyboardHidden = 2,
                       navigation = 1, navigationHidden = 2;
  public int densityDpi = 320, screenWidthDp = 960, screenHeightDp = 540,
             smallestScreenWidthDp = 540;
  public Locale locale = Locale.US;
  public Configuration() {}
  public Configuration(Configuration c) {
    setTo(c);
  }
  public void setTo(Configuration c) {
    fontScale = c.fontScale;
    colorMode = c.colorMode;
    fontWeightAdjustment = c.fontWeightAdjustment;
    mcc = c.mcc;
    mnc = c.mnc;
    orientation = c.orientation;
    uiMode = c.uiMode;
    screenLayout = c.screenLayout;
    touchscreen = c.touchscreen;
    keyboard = c.keyboard;
    keyboardHidden = c.keyboardHidden;
    hardKeyboardHidden = c.hardKeyboardHidden;
    navigation = c.navigation;
    navigationHidden = c.navigationHidden;
    densityDpi = c.densityDpi;
    screenWidthDp = c.screenWidthDp;
    screenHeightDp = c.screenHeightDp;
    smallestScreenWidthDp = c.smallestScreenWidthDp;
    locale = c.locale;
  }
  public int updateFrom(Configuration c) {
    int changed = 0;
    if (c.hardKeyboardHidden != 0 && hardKeyboardHidden != c.hardKeyboardHidden) {
      hardKeyboardHidden = c.hardKeyboardHidden; changed |= 0x20;
    }
    if (c.fontWeightAdjustment != Integer.MAX_VALUE && fontWeightAdjustment != c.fontWeightAdjustment) {
      fontWeightAdjustment = c.fontWeightAdjustment; changed |= 0x10000000;
    }
    if (c.colorMode != 0 && colorMode != c.colorMode) { colorMode = c.colorMode; changed |= 0x4000; }
    if (c.fontScale > 0 && fontScale != c.fontScale) {
      fontScale = c.fontScale;
      changed |= 0x40000000;
    }
    if (c.locale != null && !c.locale.equals(locale)) {
      locale = c.locale;
      changed |= 4;
    }
    if (c.orientation != 0 && orientation != c.orientation) {
      orientation = c.orientation;
      changed |= 128;
    }
    if (c.uiMode != 0 && uiMode != c.uiMode) {
      uiMode = c.uiMode;
      changed |= 512;
    }
    if (c.densityDpi != 0 && densityDpi != c.densityDpi) {
      densityDpi = c.densityDpi;
      changed |= 4096;
    }
    if (c.screenWidthDp != 0)
      screenWidthDp = c.screenWidthDp;
    if (c.screenHeightDp != 0)
      screenHeightDp = c.screenHeightDp;
    if (c.smallestScreenWidthDp != 0)
      smallestScreenWidthDp = c.smallestScreenWidthDp;
    return changed;
  }
  public int diff(Configuration c) {
    return new Configuration(this).updateFrom(c);
  }
  public boolean equals(Configuration c) {
    return c != null && fontWeightAdjustment == c.fontWeightAdjustment && colorMode == c.colorMode && fontScale == c.fontScale && mcc == c.mcc && mnc == c.mnc
        && orientation == c.orientation && uiMode == c.uiMode && screenLayout == c.screenLayout
        && touchscreen == c.touchscreen && keyboard == c.keyboard
        && keyboardHidden == c.keyboardHidden && hardKeyboardHidden == c.hardKeyboardHidden && navigation == c.navigation
        && navigationHidden == c.navigationHidden && densityDpi == c.densityDpi
        && screenWidthDp == c.screenWidthDp && screenHeightDp == c.screenHeightDp
        && smallestScreenWidthDp == c.smallestScreenWidthDp
        && java.util.Objects.equals(locale, c.locale);
  }
  public boolean equals(Object c) {
    return c instanceof Configuration config && equals(config);
  }
  public int hashCode() {
    return java.util.Objects.hash(fontWeightAdjustment, colorMode, fontScale, mcc, mnc, orientation, uiMode, screenLayout,
        touchscreen, keyboard, keyboardHidden, hardKeyboardHidden, navigation, navigationHidden, densityDpi,
        screenWidthDp, screenHeightDp, smallestScreenWidthDp, locale);
  }
  public void setLocale(Locale value) {
    locale = value;
  }
  public android.os.LocaleList getLocales() {
    return new android.os.LocaleList(locale);
  }
  public int getLayoutDirection() {
    return java.util.Set.of("ar", "fa", "he", "ur").contains(locale.getLanguage()) ? 1 : 0;
  }
}
