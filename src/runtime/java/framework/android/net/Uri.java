package android.net;
public final class Uri implements android.os.Parcelable, Comparable<Uri> {
  public static final Uri EMPTY = parse("");
  private final String text;
  // AOSP Builder returns HierarchicalUri/OpaqueUri: their components cannot be
  // recovered by reparsing (e.g. a hierarchical "mailto:?subject=x").
  private record Parts(String scheme, String authority, String path, String query, String fragment, String opaque) {}
  private final Parts parts;
  private Uri(String s) {
    this(s, null);
  }
  private Uri(String s, Parts parts) {
    text = s; this.parts = parts;
  }
  public static Uri parse(String s) {
    return new Uri(java.util.Objects.requireNonNull(s));
  }
  public static Uri fromFile(java.io.File f) {
    return new Builder().scheme("file").authority("").path(f.getAbsolutePath()).build();
  }
  public String toString() {
    return text;
  }
  public String getScheme() {
    if (parts != null) return parts.scheme;
    int colon = text.indexOf(':');
    return colon < 0 ? null : text.substring(0, colon);
  }
  // AOSP 12L Uri.StringUri parses separators without RFC syntax validation.
  // Encoded accessors preserve even spaces, malformed '%' escapes and braces.
  private int start() { return parts == null ? text.indexOf(':') + 1 : parts.scheme == null ? 0 : parts.scheme.length() + 1; }
  private int fragmentStart() { return text.indexOf('#', Math.max(0, start() - 1)); }
  private int endAt(int from, String delimiters) {
    int end = from;
    while (end < text.length() && delimiters.indexOf(text.charAt(end)) < 0) end++;
    return end;
  }
  public boolean isRelative() { return start() == 0; }
  public boolean isAbsolute() { return !isRelative(); }
  public boolean isHierarchical() { return parts != null ? parts.opaque == null : isRelative() || start() < text.length() && text.charAt(start()) == '/'; }
  public boolean isOpaque() { return !isHierarchical(); }
  public String getEncodedSchemeSpecificPart() {
    if (parts != null) return parts.opaque != null ? parts.opaque
        : text.substring(start(), text.length() - (parts.fragment == null || parts.fragment.isEmpty() ? 0 : parts.fragment.length() + 1));
    int end = fragmentStart();
    return text.substring(start(), end < 0 ? text.length() : end);
  }
  public String getSchemeSpecificPart() { return decode(getEncodedSchemeSpecificPart()); }
  public String getEncodedAuthority() {
    if (parts != null) return isOpaque() ? null : parts.authority;
    int start = start();
    return text.startsWith("//", start) ? text.substring(start + 2, endAt(start + 2, "/\\?#")) : null;
  }
  public String getAuthority() { return decode(getEncodedAuthority()); }
  private static int portSeparator(String authority) {
    if (authority != null) for (int i = authority.length() - 1; i >= 0; i--) {
      char c = authority.charAt(i);
      if (c == ':') return i;
      if (c < '0' || c > '9') return -1;
    }
    return -1;
  }
  public String getHost() {
    String authority = getEncodedAuthority();
    if (authority == null) return null;
    int port = portSeparator(authority);
    return decode(authority.substring(authority.lastIndexOf('@') + 1, port < 0 ? authority.length() : port));
  }
  public int getPort() {
    String authority = getEncodedAuthority();
    int port = portSeparator(authority);
    if (port < 0) return -1;
    try { return Integer.parseInt(decode(authority.substring(port + 1))); }
    catch (NumberFormatException invalid) { return -1; }
  }
  public String getEncodedUserInfo() {
    String authority = getEncodedAuthority();
    int split = authority == null ? -1 : authority.lastIndexOf('@');
    return split < 0 ? null : authority.substring(0, split);
  }
  public String getUserInfo() { return decode(getEncodedUserInfo()); }
  public String getEncodedPath() {
    if (isOpaque()) return null;
    if (parts != null) return parts.path;
    int from = start();
    if (text.startsWith("//", from)) from = endAt(from + 2, "/\\?#");
    return text.substring(from, endAt(from, "?#"));
  }
  public String getPath() { return decode(getEncodedPath()); }
  public String getEncodedQuery() {
    if (parts != null) return isOpaque() ? null : parts.query;
    int query = text.indexOf('?', Math.max(0, start() - 1)), fragment = fragmentStart();
    return query < 0 || fragment >= 0 && fragment < query ? null
        : text.substring(query + 1, fragment < 0 ? text.length() : fragment);
  }
  public String getQuery() { return decode(getEncodedQuery()); }
  public String getEncodedFragment() {
    if (parts != null) return parts.fragment;
    int i = fragmentStart(); return i < 0 ? null : text.substring(i + 1);
  }
  public String getFragment() { return decode(getEncodedFragment()); }
  public java.util.List<String> getPathSegments() {
    String path = getEncodedPath();
    if (path == null || path.isEmpty()) return java.util.List.of();
    var segments = new java.util.ArrayList<String>();
    for (String segment : path.split("/")) if (!segment.isEmpty()) segments.add(decode(segment));
    return java.util.Collections.unmodifiableList(segments);
  }
  public String getLastPathSegment() {
    var segments = getPathSegments();
    return segments.isEmpty() ? null : segments.get(segments.size() - 1);
  }
  @Override public int compareTo(Uri other) { return text.compareTo(other.text); }
  public static Uri fromParts(String scheme, String ssp, String fragment) {
    return new Builder().scheme(java.util.Objects.requireNonNull(scheme))
        .opaquePart(java.util.Objects.requireNonNull(ssp)).fragment(fragment).build();
  }
  public static Uri withAppendedPath(Uri base, String path) { return base.buildUpon().appendEncodedPath(path).build(); }
  public Builder buildUpon() {
    var builder = new Builder().scheme(getScheme()).encodedFragment(getEncodedFragment());
    return isOpaque() ? builder.encodedOpaquePart(getEncodedSchemeSpecificPart())
        : builder.encodedAuthority(getEncodedAuthority()).encodedPath(getEncodedPath()).encodedQuery(getEncodedQuery());
  }
  public java.util.Set<String> getQueryParameterNames() {
    if (isOpaque()) throw new UnsupportedOperationException("This isn't a hierarchical URI.");
    String query = getEncodedQuery();
    if (query == null) return java.util.Set.of();
    var names = new java.util.LinkedHashSet<String>();
    for (String part : query.split("&")) {
      if (part.isEmpty()) continue;
      int equals = part.indexOf('=');
      names.add(decode(equals < 0 ? part : part.substring(0, equals)));
    }
    return java.util.Collections.unmodifiableSet(names);
  }
  public java.util.List<String> getQueryParameters(String key) {
    if (isOpaque()) throw new UnsupportedOperationException("This isn't a hierarchical URI.");
    java.util.Objects.requireNonNull(key);
    String query = getEncodedQuery();
    if (query == null) return java.util.List.of();
    String encodedKey = encode(key);
    var values = new java.util.ArrayList<String>();
    for (String part : query.split("&", -1)) {
      int equals = part.indexOf('=');
      if ((equals < 0 ? part : part.substring(0, equals)).equals(encodedKey))
        values.add(equals < 0 ? "" : decode(part.substring(equals + 1)));
    }
    return java.util.Collections.unmodifiableList(values);
  }
  public String getQueryParameter(String name) {
    if (isOpaque()) throw new UnsupportedOperationException("This isn't a hierarchical URI.");
    java.util.Objects.requireNonNull(name);
    String query = getEncodedQuery();
    if (query == null) return null;
    String encodedName = encode(name);
    for (String part : query.split("&", -1)) {
      int equals = part.indexOf('=');
      if ((equals < 0 ? part : part.substring(0, equals)).equals(encodedName))
        return equals < 0 ? "" : decode(part.substring(equals + 1).replace("+", " "));
    }
    return null;
  }
  public static final class Builder {
    private String scheme, authority, path, query, fragment, opaque;
    public Builder() {}
    public Builder scheme(String value) { scheme = value; return this; }
    public Builder authority(String value) { return encodedAuthority(encode(value)); }
    public Builder encodedAuthority(String value) { authority = value; opaque = null; return this; }
    public Builder path(String value) { return encodedPath(encode(value, "/")); }
    public Builder encodedPath(String value) { path = value; opaque = null; return this; }
    public Builder query(String value) { return encodedQuery(encode(value)); }
    public Builder encodedQuery(String value) { query = value; opaque = null; return this; }
    public Builder fragment(String value) { return encodedFragment(encode(value)); }
    public Builder encodedFragment(String value) { fragment = value; return this; }
    public Builder opaquePart(String value) { return encodedOpaquePart(encode(value)); }
    public Builder encodedOpaquePart(String value) { opaque = value; return this; }
    public Builder appendPath(String value) { return appendEncodedPath(encode(value)); }
    public Builder appendEncodedPath(String value) {
      return encodedPath(path == null || path.isEmpty() ? "/" + value : path + (path.endsWith("/") ? "" : "/") + value);
    }
    public Builder appendQueryParameter(String name, String value) {
      String part = encode(name) + "=" + encode(value);
      return encodedQuery(query == null || query.isEmpty() ? part : query + "&" + part);
    }
    public Builder clearQuery() { return encodedQuery(null); }
    public Uri build() {
      StringBuilder result = new StringBuilder();
      String builtPath = path == null ? "" : path;
      if (scheme != null) result.append(scheme).append(':');
      if (opaque != null) {
        if (scheme == null) throw new UnsupportedOperationException("An opaque URI must have a scheme.");
        result.append(opaque);
      } else {
        if (authority != null) result.append("//").append(authority);
        if (!builtPath.isEmpty()) {
          if ((scheme != null || authority != null) && !builtPath.startsWith("/")) builtPath = '/' + builtPath;
          result.append(builtPath);
        }
        if (query != null && !query.isEmpty()) result.append('?').append(query);
      }
      if (fragment != null && !fragment.isEmpty()) result.append('#').append(fragment);
      return new Uri(result.toString(), new Parts(scheme, authority, builtPath, query, fragment, opaque));
    }
    @Override public String toString() { return build().toString(); }
  }
  public boolean equals(Object o) {
    return o instanceof Uri u && text.equals(u.text);
  }
  public int hashCode() {
    return text.hashCode();
  }
  public int describeContents() {
    return 0;
  }
  public void writeToParcel(android.os.Parcel p, int f) {
    p.writeString(text);
  }
  public static final android.os.Parcelable.Creator<Uri> CREATOR = new android.os.Parcelable.Creator<>() {
    public Uri createFromParcel(android.os.Parcel parcel) { return parse(parcel.readString()); }
    public Uri[] newArray(int size) { return new Uri[size]; }
  };
  public static String encode(String s) {
    return encode(s, null);
  }
  public static String encode(String s, String allow) {
    if (s == null) return null;
    // AOSP android/net/Uri.java: URI escaping, not HTML form encoding.
    StringBuilder out = new StringBuilder();
    final String hex = "0123456789ABCDEF";
    for (byte b : s.getBytes(java.nio.charset.StandardCharsets.UTF_8)) {
      int c = b & 255;
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || "_-!.~'()*".indexOf(c) >= 0 ||
          (c < 128 && allow != null && allow.indexOf(c) >= 0))
        out.append((char)c);
      else
        out.append('%').append(hex.charAt(c >> 4)).append(hex.charAt(c & 15));
    }
    return out.toString();
  }
  public static String decode(String s) {
    if (s == null) return null;
    // AOSP 12L UriCodec's non-throwing decoder consumes malformed escapes
    // one digit at a time, retaining the partial byte and replacing bad input.
    StringBuilder result = new StringBuilder();
    var bytes = new java.io.ByteArrayOutputStream();
    for (int i = 0; i < s.length();) {
      char c = s.charAt(i++);
      if (c != '%') {
        result.append(bytes.toString(java.nio.charset.StandardCharsets.UTF_8)); bytes.reset();
        result.append(c); continue;
      }
      int value = 0;
      for (int digit = 0; digit < 2; digit++) {
        if (i == s.length()) {
          result.append(bytes.toString(java.nio.charset.StandardCharsets.UTF_8)).append('\ufffd');
          return result.toString();
        }
        c = s.charAt(i++);
        int hex = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (hex < 0) {
          result.append(bytes.toString(java.nio.charset.StandardCharsets.UTF_8)).append('\ufffd'); bytes.reset();
          break;
        }
        value = value * 16 + hex;
      }
      bytes.write(value);
    }
    return result.append(bytes.toString(java.nio.charset.StandardCharsets.UTF_8)).toString();
  }
}
