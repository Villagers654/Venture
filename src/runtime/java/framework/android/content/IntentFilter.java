package android.content;

import android.net.Uri;
import android.os.PatternMatcher;
import java.util.*;

/** AOSP 12L IntentFilter action/category/data matching for installed packages. */
public class IntentFilter {
  public static final int NO_MATCH_TYPE = -1, NO_MATCH_DATA = -2, NO_MATCH_ACTION = -3, NO_MATCH_CATEGORY = -4;
  public static final int MATCH_CATEGORY_EMPTY = 0x100000, MATCH_CATEGORY_SCHEME = 0x200000,
      MATCH_CATEGORY_HOST = 0x300000, MATCH_CATEGORY_PORT = 0x400000, MATCH_CATEGORY_PATH = 0x500000,
      MATCH_CATEGORY_SCHEME_SPECIFIC_PART = 0x580000, MATCH_CATEGORY_TYPE = 0x600000,
      MATCH_ADJUSTMENT_NORMAL = 0x8000, MATCH_CATEGORY_MASK = 0xfff0000;
  public static class MalformedMimeTypeException extends android.util.AndroidException {
    public MalformedMimeTypeException(String type) { super(type); }
  }
  private final List<String> actions = new ArrayList<>(), categories = new ArrayList<>(),
      schemes = new ArrayList<>(), types = new ArrayList<>();
  private final List<AuthorityEntry> authorities = new ArrayList<>();
  private final List<PatternMatcher> paths = new ArrayList<>(), parts = new ArrayList<>();
  private int priority;
  public IntentFilter() {}
  public IntentFilter(String action) { addAction(action); }
  public IntentFilter(String action, String type) throws MalformedMimeTypeException { addAction(action); addDataType(type); }
  public IntentFilter(IntentFilter other) {
    actions.addAll(other.actions); categories.addAll(other.categories); schemes.addAll(other.schemes);
    types.addAll(other.types); authorities.addAll(other.authorities); paths.addAll(other.paths);
    parts.addAll(other.parts); priority = other.priority;
  }
  public void addAction(String action) { if (!actions.contains(Objects.requireNonNull(action))) actions.add(action); }
  public boolean hasAction(String action) { return actions.contains(action); }
  public boolean matchAction(String action) { return hasAction(action); }
  public int countActions() { return actions.size(); }
  public String getAction(int index) { return actions.get(index); }
  public Iterator<String> actionsIterator() { return actions.iterator(); }
  public void addCategory(String category) { if (!categories.contains(Objects.requireNonNull(category))) categories.add(category); }
  public boolean hasCategory(String category) { return categories.contains(category); }
  public int countCategories() { return categories.size(); }
  public String getCategory(int index) { return categories.get(index); }
  public Iterator<String> categoriesIterator() { return categories.isEmpty() ? null : categories.iterator(); }
  public void setPriority(int priority) { this.priority = priority; }
  public int getPriority() { return priority; }
  public void addDataScheme(String scheme) { if (!schemes.contains(Objects.requireNonNull(scheme))) schemes.add(scheme); }
  public int countDataSchemes() { return schemes.size(); }
  public String getDataScheme(int index) { return schemes.get(index); }
  public boolean hasDataScheme(String scheme) { return schemes.contains(scheme); }
  public void addDataType(String type) throws MalformedMimeTypeException {
    int slash = type.indexOf('/');
    if (slash <= 0 || slash == type.length() - 1) throw new MalformedMimeTypeException(type);
    if (!types.contains(type)) types.add(type);
  }
  public int countDataTypes() { return types.size(); }
  public String getDataType(int index) {
    String type = types.get(index);
    return type.endsWith("/*") ? type.substring(0, type.length() - 2) : type;
  }
  public boolean hasDataType(String type) {
    if (type == null) return false;
    if (types.contains(type) || !types.isEmpty() && type.equals("*/*") || types.contains("*/*")) return true;
    int slash = type.indexOf('/');
    if (slash <= 0) return false;
    String prefix = type.substring(0, slash + 1);
    return types.contains(prefix + "*") || type.endsWith("/*") && types.stream().anyMatch(t -> t.startsWith(prefix));
  }
  public static final class AuthorityEntry {
    private final String host;
    private final int port;
    public AuthorityEntry(String host, String port) {
      this.host = Objects.requireNonNull(host); this.port = port == null ? -1 : Integer.parseInt(port);
    }
    public String getHost() { return host; }
    public int getPort() { return port; }
    public int match(Uri data) {
      if (data == null || data.getHost() == null) return NO_MATCH_DATA;
      String candidate = data.getHost(), expected = host;
      if (host.startsWith("*")) {
        expected = host.substring(1);
        if (candidate.length() < expected.length()) return NO_MATCH_DATA;
        candidate = candidate.substring(candidate.length() - expected.length());
      }
      if (!candidate.equalsIgnoreCase(expected)) return NO_MATCH_DATA;
      return port < 0 ? MATCH_CATEGORY_HOST : port == data.getPort() ? MATCH_CATEGORY_PORT : NO_MATCH_DATA;
    }
  }
  public void addDataAuthority(String host, String port) { authorities.add(new AuthorityEntry(host, port)); }
  public int countDataAuthorities() { return authorities.size(); }
  public AuthorityEntry getDataAuthority(int index) { return authorities.get(index); }
  public int matchDataAuthority(Uri data) {
    for (var authority : authorities) { int match = authority.match(data); if (match >= 0) return match; }
    return NO_MATCH_DATA;
  }
  public void addDataPath(String path, int type) { paths.add(new PatternMatcher(path, type)); }
  public int countDataPaths() { return paths.size(); }
  public PatternMatcher getDataPath(int index) { return paths.get(index); }
  public boolean hasDataPath(String path) { return paths.stream().anyMatch(p -> p.match(path)); }
  public void addDataSchemeSpecificPart(String part, int type) { parts.add(new PatternMatcher(part, type)); }
  public int countDataSchemeSpecificParts() { return parts.size(); }
  public PatternMatcher getDataSchemeSpecificPart(int index) { return parts.get(index); }
  public boolean hasDataSchemeSpecificPart(String part) { return parts.stream().anyMatch(p -> p.match(part)); }
  public int matchData(String type, String scheme, Uri data) {
    int match = MATCH_CATEGORY_EMPTY;
    if (types.isEmpty() && schemes.isEmpty())
      return type == null && data == null ? match + MATCH_ADJUSTMENT_NORMAL : NO_MATCH_DATA;
    if (!schemes.isEmpty()) {
      if (!schemes.contains(scheme == null ? "" : scheme)) return NO_MATCH_DATA;
      match = MATCH_CATEGORY_SCHEME;
      if (!parts.isEmpty() && data != null)
        match = hasDataSchemeSpecificPart(data.getSchemeSpecificPart()) ? MATCH_CATEGORY_SCHEME_SPECIFIC_PART : NO_MATCH_DATA;
      if (match != MATCH_CATEGORY_SCHEME_SPECIFIC_PART && !authorities.isEmpty()) {
        int authority = matchDataAuthority(data);
        if (authority < 0) return NO_MATCH_DATA;
        if (paths.isEmpty()) match = authority;
        else if (hasDataPath(data.getPath())) match = MATCH_CATEGORY_PATH;
        else return NO_MATCH_DATA;
      }
      if (match == NO_MATCH_DATA) return match;
    } else if (scheme != null && !scheme.isEmpty() && !scheme.equals("content") && !scheme.equals("file")) {
      return NO_MATCH_DATA;
    }
    if (!types.isEmpty()) {
      if (!hasDataType(type)) return NO_MATCH_TYPE;
      match = MATCH_CATEGORY_TYPE;
    } else if (type != null) return NO_MATCH_TYPE;
    return match + MATCH_ADJUSTMENT_NORMAL;
  }
  public String matchCategories(Set<String> requested) {
    if (requested != null) for (String category : requested) if (!categories.contains(category)) return category;
    return null;
  }
  public int match(String action, String type, String scheme, Uri data, Set<String> categories, String logTag) {
    if (action != null && !hasAction(action)) return NO_MATCH_ACTION;
    int match = matchData(type, scheme, data);
    if (match < 0) return match;
    return matchCategories(categories) == null ? match : NO_MATCH_CATEGORY;
  }
  public int match(ContentResolver resolver, Intent intent, boolean resolve, String logTag) {
    return match(intent.getAction(), resolve ? intent.resolveType(resolver) : intent.getType(),
        intent.getScheme(), intent.getData(), intent.getCategories(), logTag);
  }
}
