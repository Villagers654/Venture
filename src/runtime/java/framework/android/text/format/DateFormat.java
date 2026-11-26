package android.text.format;

import android.content.Context;
import java.time.chrono.IsoChronology;
import java.time.format.DateTimeFormatterBuilder;
import java.time.format.FormatStyle;
import java.util.Calendar;
import java.util.Date;
import java.util.Locale;

// Date and time formats for the device's locale. A Quest has no 12/24-hour
// setting of its own, so the locale's time format decides, as Android does
// while the setting is unset.
public class DateFormat {
  public DateFormat() {}

  public static boolean is24HourFormat(Context context) {
    return is24HourLocale(Locale.getDefault());
  }

  public static boolean is24HourFormat(Context context, int userHandle) {
    return is24HourFormat(context);
  }

  static boolean is24HourLocale(Locale locale) {
    final String pattern = DateTimeFormatterBuilder.getLocalizedDateTimePattern(
        null, FormatStyle.SHORT, IsoChronology.INSTANCE, locale);
    return pattern.indexOf('H') >= 0 || pattern.indexOf('k') >= 0;
  }

  // The locale's pattern with the fields of `skeleton` (CLDR skeleton letters).
  public static String getBestDateTimePattern(Locale locale, String skeleton) {
    try {
      return DateTimeFormatterBuilder.getLocalizedDateTimePattern(skeleton, IsoChronology.INSTANCE, locale);
    } catch (RuntimeException e) {
      return skeleton;
    }
  }

  public static java.text.DateFormat getTimeFormat(Context context) {
    return new java.text.SimpleDateFormat(is24HourFormat(context) ? "HH:mm" : "h:mm a", Locale.getDefault());
  }

  public static java.text.DateFormat getDateFormat(Context context) {
    return java.text.DateFormat.getDateInstance(java.text.DateFormat.SHORT, Locale.getDefault());
  }

  public static java.text.DateFormat getMediumDateFormat(Context context) {
    return java.text.DateFormat.getDateInstance(java.text.DateFormat.MEDIUM, Locale.getDefault());
  }

  public static java.text.DateFormat getLongDateFormat(Context context) {
    return java.text.DateFormat.getDateInstance(java.text.DateFormat.LONG, Locale.getDefault());
  }

  // The order of day, month and year in the locale's short date ('d', 'M', 'y').
  public static char[] getDateFormatOrder(Context context) {
    final String pattern = DateTimeFormatterBuilder.getLocalizedDateTimePattern(
        FormatStyle.SHORT, null, IsoChronology.INSTANCE, Locale.getDefault());
    final char[] order = new char[3];
    int n = 0;
    boolean quoted = false;
    for (int i = 0; i < pattern.length() && n < 3; ++i) {
      char c = pattern.charAt(i);
      if (c == '\'') {
        quoted = !quoted;
        continue;
      }
      if (quoted) continue;
      if (c == 'L') c = 'M';
      if ((c == 'd' || c == 'M' || c == 'y') && new String(order, 0, n).indexOf(c) < 0) order[n++] = c;
    }
    return n == 3 ? order : new char[] {'M', 'd', 'y'};
  }

  public static CharSequence format(CharSequence inFormat, long inTimeInMillis) {
    return format(inFormat, new Date(inTimeInMillis));
  }

  public static CharSequence format(CharSequence inFormat, Date inDate) {
    return pattern(inFormat).format(inDate);
  }

  public static CharSequence format(CharSequence inFormat, Calendar inDate) {
    final java.text.SimpleDateFormat f = pattern(inFormat);
    f.setTimeZone(inDate.getTimeZone());
    return f.format(inDate.getTime());
  }

  // Android's letters are SimpleDateFormat's, but for 'c' (stand-alone day of
  // the week), which SimpleDateFormat spells 'E'.
  private static java.text.SimpleDateFormat pattern(CharSequence inFormat) {
    final StringBuilder p = new StringBuilder(inFormat.length());
    boolean quoted = false;
    for (int i = 0; i < inFormat.length(); ++i) {
      final char c = inFormat.charAt(i);
      if (c == '\'') quoted = !quoted;
      p.append(!quoted && c == 'c' ? 'E' : c);
    }
    return new java.text.SimpleDateFormat(p.toString(), Locale.getDefault());
  }
}
