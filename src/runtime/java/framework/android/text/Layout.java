package android.text;

/** Type contract for optional text-layout paths; no layout producer is exposed. */
public abstract class Layout {
  // AOSP android12-release android/text/Layout.java. AppCompat's autosizing
  // helper must verify this hierarchy even when autosizing is disabled.
  public enum Alignment { ALIGN_NORMAL, ALIGN_OPPOSITE, ALIGN_CENTER, ALIGN_LEFT, ALIGN_RIGHT }
}
