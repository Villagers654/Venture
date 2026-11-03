package android.content;

import android.net.Uri;
import java.util.ArrayList;
import java.util.List;

/**
 * Android's authority/path matcher. "#" matches a segment of digits, "*" any segment; the
 * first matching child in insertion order wins, as on Android.
 */
public class UriMatcher {
  public static final int NO_MATCH = -1;
  private static final int EXACT = 0, NUMBER = 1, TEXT = 2;

  private int code;
  private final int which;
  private final String text;
  private List<UriMatcher> children;

  public UriMatcher(int code) {
    this(code, EXACT, null);
  }

  private UriMatcher(int code, int which, String text) {
    this.code = code;
    this.which = which;
    this.text = text;
  }

  public void addURI(String authority, String path, int code) {
    if (code < 0) throw new IllegalArgumentException("code " + code + " is invalid: it must be positive");
    String[] tokens = null;
    if (path != null) {
      String trimmed = path.length() > 1 && path.charAt(0) == '/' ? path.substring(1) : path;
      tokens = trimmed.split("/");
    }
    int count = tokens != null ? tokens.length : 0;
    UriMatcher node = this;
    for (int i = -1; i < count; i++) {
      String token = i < 0 ? authority : tokens[i];
      if (node.children == null) node.children = new ArrayList<>();
      UriMatcher next = null;
      for (UriMatcher child : node.children) {
        if (token.equals(child.text)) {
          next = child;
          break;
        }
      }
      if (next == null) {
        int kind = token.equals("#") ? NUMBER : token.equals("*") ? TEXT : EXACT;
        next = new UriMatcher(NO_MATCH, kind, token);
        node.children.add(next);
      }
      node = next;
    }
    node.code = code;
  }

  public int match(Uri uri) {
    List<String> segments = uri.getPathSegments();
    String authority = uri.getAuthority();
    if (segments.isEmpty() && authority == null) return code;
    UriMatcher node = this;
    for (int i = -1; i < segments.size(); i++) {
      String token = i < 0 ? authority : segments.get(i);
      if (node.children == null) break;
      UriMatcher next = null;
      for (UriMatcher child : node.children) {
        if (child.matches(token)) {
          next = child;
          break;
        }
      }
      if (next == null) return NO_MATCH;
      node = next;
    }
    return node.code;
  }

  private boolean matches(String token) {
    if (token == null) return false;
    switch (which) {
      case EXACT:
        return text.equals(token);
      case NUMBER:
        for (int i = 0; i < token.length(); i++) {
          char c = token.charAt(i);
          if (c < '0' || c > '9') return false;
        }
        return true;
      default:
        return true;
    }
  }
}
