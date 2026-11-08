package android.text.style;

import android.content.Context;
import java.util.Arrays;
import java.util.Locale;

/** Suggestion metadata attached to an editable text range. */
public class SuggestionSpan {
  public static final int FLAG_EASY_CORRECT = 1;
  public static final int FLAG_MISSPELLED = 2;
  public static final int FLAG_AUTO_CORRECTION = 4;
  public static final int FLAG_GRAMMAR_ERROR = 8;
  public static final int SUGGESTIONS_MAX_SIZE = 5;

  private final Locale locale;
  private final String[] suggestions;
  private int flags;

  public SuggestionSpan(Context context, String[] suggestions, int flags) {
    this(context, null, suggestions, flags, null);
  }

  public SuggestionSpan(Locale locale, String[] suggestions, int flags) {
    this(null, locale, suggestions, flags, null);
  }

  public SuggestionSpan(Context context, Locale locale, String[] suggestions, int flags,
                        Class<?> notificationTargetClass) {
    this.locale = locale != null || context == null ? locale : context.getResources().getConfiguration().locale;
    this.suggestions = Arrays.copyOf(suggestions, Math.min(SUGGESTIONS_MAX_SIZE, suggestions.length));
    this.flags = flags;
  }

  // shortcut: metadata only until the framework supports rendering and parceling text styles.
  public String[] getSuggestions() { return suggestions; }
  public String getLocale() { return locale == null ? "" : locale.toString(); }
  public Locale getLocaleObject() { return locale; }
  public int getFlags() { return flags; }
  public void setFlags(int flags) { this.flags = flags; }
}
