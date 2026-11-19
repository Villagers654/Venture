package android.util;

import java.io.Closeable;
import java.io.EOFException;
import java.io.IOException;
import java.io.Reader;
import java.util.ArrayList;
import java.util.List;

/**
 * Android 3.0's streaming JSON decoder. Strict mode takes RFC 4627 JSON: one top-level array or
 * object, double-quoted names and strings, JSON numbers. Lenient mode also takes what Android's
 * reader does: several or bare top-level values, comments (slash-star, slash-slash, '#'),
 * single-quoted and unquoted names and strings, '=' and '=>' after names, ';' between members,
 * omitted array elements as null, NaN and infinities, and a leading ")]}'" line.
 * A failed nextX() leaves the token in place, so the caller can read it another way.
 */
public final class JsonReader implements Closeable {
  private enum Scope { EMPTY_ARRAY, NONEMPTY_ARRAY, EMPTY_OBJECT, DANGLING_NAME, NONEMPTY_OBJECT, EMPTY_DOCUMENT, NONEMPTY_DOCUMENT, CLOSED }
  private static final String NON_EXECUTE_PREFIX = ")]}'\n";

  private final Reader in;
  private final char[] buffer = new char[1024];
  private int pos, limit;
  // Characters given back after reading ahead, last in first out.
  private final StringBuilder pushback = new StringBuilder();
  private int line = 1, column = 1;
  private boolean lenient;
  private final List<Scope> stack = new ArrayList<>();
  private JsonToken token;
  private String name;
  private String value;

  public JsonReader(Reader in) {
    if (in == null) throw new NullPointerException("in == null");
    this.in = in;
    stack.add(Scope.EMPTY_DOCUMENT);
  }

  public void setLenient(boolean lenient) {
    this.lenient = lenient;
  }

  public boolean isLenient() {
    return lenient;
  }

  public void beginArray() throws IOException {
    expect(JsonToken.BEGIN_ARRAY);
  }

  public void endArray() throws IOException {
    expect(JsonToken.END_ARRAY);
  }

  public void beginObject() throws IOException {
    expect(JsonToken.BEGIN_OBJECT);
  }

  public void endObject() throws IOException {
    expect(JsonToken.END_OBJECT);
  }

  private void expect(JsonToken expected) throws IOException {
    peek();
    if (token != expected) throw new IllegalStateException("Expected " + expected + " but was " + peek());
    advance();
  }

  public boolean hasNext() throws IOException {
    peek();
    return token != JsonToken.END_OBJECT && token != JsonToken.END_ARRAY;
  }

  public JsonToken peek() throws IOException {
    if (token != null) return token;
    switch (top()) {
      case EMPTY_DOCUMENT:
        if (lenient) consumeNonExecutePrefix();
        replaceTop(Scope.NONEMPTY_DOCUMENT);
        JsonToken first = nextValue();
        if (!lenient && first != JsonToken.BEGIN_ARRAY && first != JsonToken.BEGIN_OBJECT)
          throw syntaxError("Expected JSON document to start with '[' or '{'");
        return first;
      case EMPTY_ARRAY: return nextInArray(true);
      case NONEMPTY_ARRAY: return nextInArray(false);
      case EMPTY_OBJECT: return nextInObject(true);
      case DANGLING_NAME: return objectValue();
      case NONEMPTY_OBJECT: return nextInObject(false);
      case NONEMPTY_DOCUMENT:
        int c = nextNonWhitespace(false);
        if (c == -1) return token = JsonToken.END_DOCUMENT;
        unread(c);
        if (!lenient) throw syntaxError("Expected EOF");
        return nextValue();
      case CLOSED:
      default:
        throw new IllegalStateException("JsonReader is closed");
    }
  }

  private JsonToken advance() throws IOException {
    peek();
    JsonToken result = token;
    token = null;
    value = null;
    name = null;
    return result;
  }

  public String nextName() throws IOException {
    peek();
    if (token != JsonToken.NAME) throw new IllegalStateException("Expected a name but was " + peek());
    String result = name;
    advance();
    return result;
  }

  /** A STRING token's text, or a NUMBER token's literal. */
  public String nextString() throws IOException {
    peek();
    if (token != JsonToken.STRING && token != JsonToken.NUMBER)
      throw new IllegalStateException("Expected a string but was " + peek());
    String result = value;
    advance();
    return result;
  }

  public boolean nextBoolean() throws IOException {
    peek();
    if (token != JsonToken.BOOLEAN) throw new IllegalStateException("Expected a boolean but was " + token);
    boolean result = value.equalsIgnoreCase("true");
    advance();
    return result;
  }

  public void nextNull() throws IOException {
    peek();
    if (token != JsonToken.NULL) throw new IllegalStateException("Expected null but was " + token);
    advance();
  }

  /** A NUMBER or a STRING that parses as a double. */
  public double nextDouble() throws IOException {
    peek();
    if (token != JsonToken.STRING && token != JsonToken.NUMBER)
      throw new IllegalStateException("Expected a double but was " + token);
    double result = Double.parseDouble(value);
    if (result >= 1.0d && value.startsWith("0")) throw syntaxError("JSON forbids octal prefixes: " + value);
    if (!lenient && (Double.isNaN(result) || Double.isInfinite(result)))
      throw syntaxError("JSON forbids NaN and infinities: " + value);
    advance();
    return result;
  }

  /** A NUMBER or STRING that is a long exactly; "1.0" and "1e3" count, "1.5" throws NumberFormatException. */
  public long nextLong() throws IOException {
    peek();
    if (token != JsonToken.STRING && token != JsonToken.NUMBER)
      throw new IllegalStateException("Expected a long but was " + token);
    long result;
    try {
      result = Long.parseLong(value);
    } catch (NumberFormatException notLong) {
      double asDouble = Double.parseDouble(value);
      result = (long) asDouble;
      if (result != asDouble) throw new NumberFormatException(value);
    }
    if (result >= 1L && value.startsWith("0")) throw syntaxError("JSON forbids octal prefixes: " + value);
    advance();
    return result;
  }

  public int nextInt() throws IOException {
    peek();
    if (token != JsonToken.STRING && token != JsonToken.NUMBER)
      throw new IllegalStateException("Expected an int but was " + token);
    int result;
    try {
      result = Integer.parseInt(value);
    } catch (NumberFormatException notInt) {
      double asDouble = Double.parseDouble(value);
      result = (int) asDouble;
      if (result != asDouble) throw new NumberFormatException(value);
    }
    if (result >= 1L && value.startsWith("0")) throw syntaxError("JSON forbids octal prefixes: " + value);
    advance();
    return result;
  }

  public void close() throws IOException {
    value = null;
    name = null;
    token = null;
    stack.clear();
    stack.add(Scope.CLOSED);
    in.close();
  }

  /** Skips the next value, nested arrays and objects included; at a NAME, skips the name. */
  public void skipValue() throws IOException {
    int count = 0;
    do {
      JsonToken skipped = advance();
      if (skipped == JsonToken.BEGIN_ARRAY || skipped == JsonToken.BEGIN_OBJECT) count++;
      else if (skipped == JsonToken.END_ARRAY || skipped == JsonToken.END_OBJECT) count--;
      else if (skipped == JsonToken.END_DOCUMENT) throw new IllegalStateException("Expected a value but was END_DOCUMENT");
    } while (count != 0);
  }

  private Scope top() {
    return stack.get(stack.size() - 1);
  }

  private void replaceTop(Scope scope) {
    stack.set(stack.size() - 1, scope);
  }

  private void pop() {
    stack.remove(stack.size() - 1);
  }

  private JsonToken nextInArray(boolean first) throws IOException {
    if (first) {
      replaceTop(Scope.NONEMPTY_ARRAY);
    } else {
      switch (nextNonWhitespace(true)) {
        case ']':
          pop();
          return token = JsonToken.END_ARRAY;
        case ';':
          checkLenient();
          break;
        case ',':
          break;
        default:
          throw syntaxError("Unterminated array");
      }
    }
    int c = nextNonWhitespace(true);
    switch (c) {
      case ']':
        if (first) {
          pop();
          return token = JsonToken.END_ARRAY;
        }
        // An omitted element before ']', as with "[1,]": lenient reads null.
      case ';':
      case ',':
        checkLenient();
        unread(c);
        value = "null";
        return token = JsonToken.NULL;
      default:
        unread(c);
        return nextValue();
    }
  }

  private JsonToken nextInObject(boolean first) throws IOException {
    if (first) {
      int c = nextNonWhitespace(true);
      if (c == '}') {
        pop();
        return token = JsonToken.END_OBJECT;
      }
      unread(c);
    } else {
      switch (nextNonWhitespace(true)) {
        case '}':
          pop();
          return token = JsonToken.END_OBJECT;
        case ';':
          checkLenient();
          break;
        case ',':
          break;
        default:
          throw syntaxError("Unterminated object");
      }
    }
    int quote = nextNonWhitespace(true);
    switch (quote) {
      case '\'':
        checkLenient();
        // fall through
      case '"':
        name = nextQuoted((char) quote);
        break;
      default:
        checkLenient();
        unread(quote);
        name = nextLiteral();
        if (name.isEmpty()) throw syntaxError("Expected name");
    }
    replaceTop(Scope.DANGLING_NAME);
    return token = JsonToken.NAME;
  }

  private JsonToken objectValue() throws IOException {
    switch (nextNonWhitespace(true)) {
      case ':':
        break;
      case '=':
        checkLenient();
        int c = read();
        if (c != '>' && c != -1) unread(c);
        break;
      default:
        throw syntaxError("Expected ':'");
    }
    replaceTop(Scope.NONEMPTY_OBJECT);
    return nextValue();
  }

  private JsonToken nextValue() throws IOException {
    int c = nextNonWhitespace(true);
    switch (c) {
      case '{':
        stack.add(Scope.EMPTY_OBJECT);
        return token = JsonToken.BEGIN_OBJECT;
      case '[':
        stack.add(Scope.EMPTY_ARRAY);
        return token = JsonToken.BEGIN_ARRAY;
      case '\'':
        checkLenient();
        // fall through
      case '"':
        value = nextQuoted((char) c);
        return token = JsonToken.STRING;
      default:
        unread(c);
        String literal = nextLiteral();
        if (literal.isEmpty()) throw syntaxError("Expected literal value");
        token = decodeLiteral(literal);
        if (token == JsonToken.STRING) checkLenient();
        value = literal;
        return token;
    }
  }

  private static JsonToken decodeLiteral(String literal) {
    if (literal.equalsIgnoreCase("null")) return JsonToken.NULL;
    if (literal.equalsIgnoreCase("true") || literal.equalsIgnoreCase("false")) return JsonToken.BOOLEAN;
    return isJsonNumber(literal) ? JsonToken.NUMBER : JsonToken.STRING;
  }

  // RFC 4627: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
  private static boolean isJsonNumber(String s) {
    int i = 0, n = s.length();
    if (i < n && s.charAt(i) == '-') i++;
    if (i >= n) return false;
    if (s.charAt(i) == '0') {
      i++;
    } else if (s.charAt(i) >= '1' && s.charAt(i) <= '9') {
      while (i < n && s.charAt(i) >= '0' && s.charAt(i) <= '9') i++;
    } else {
      return false;
    }
    if (i < n && s.charAt(i) == '.') {
      int start = ++i;
      while (i < n && s.charAt(i) >= '0' && s.charAt(i) <= '9') i++;
      if (i == start) return false;
    }
    if (i < n && (s.charAt(i) == 'e' || s.charAt(i) == 'E')) {
      i++;
      if (i < n && (s.charAt(i) == '+' || s.charAt(i) == '-')) i++;
      int start = i;
      while (i < n && s.charAt(i) >= '0' && s.charAt(i) <= '9') i++;
      if (i == start) return false;
    }
    return i == n;
  }

  // An unquoted name or value, up to a structural character, whitespace or the end.
  private String nextLiteral() throws IOException {
    StringBuilder literal = new StringBuilder();
    while (true) {
      int c = read();
      switch (c) {
        case '/':
        case '\\':
        case ';':
        case '#':
        case '=':
          checkLenient();
          // fall through
        case '{':
        case '}':
        case '[':
        case ']':
        case ':':
        case ',':
        case ' ':
        case '\t':
        case '\f':
        case '\r':
        case '\n':
          unread(c);
          return literal.toString();
        case -1:
          return literal.toString();
        default:
          literal.append((char) c);
      }
    }
  }

  private String nextQuoted(char quote) throws IOException {
    StringBuilder result = new StringBuilder();
    while (true) {
      int c = read();
      if (c == -1) throw syntaxError("Unterminated string");
      if (c == quote) return result.toString();
      if (c == '\\') result.append(readEscape());
      else result.append((char) c);
    }
  }

  private char readEscape() throws IOException {
    int c = read();
    if (c == -1) throw syntaxError("Unterminated escape sequence");
    switch (c) {
      case 'u':
        char[] hex = new char[4];
        for (int i = 0; i < 4; i++) {
          int h = read();
          if (h == -1) throw syntaxError("Unterminated escape sequence");
          hex[i] = (char) h;
        }
        String digits = new String(hex);
        for (char h : hex)
          if (Character.digit(h, 16) < 0) throw new NumberFormatException("\\u" + digits);
        return (char) Integer.parseInt(digits, 16);
      case 't': return '\t';
      case 'b': return '\b';
      case 'n': return '\n';
      case 'r': return '\r';
      case 'f': return '\f';
      default: return (char) c; // \' \" \\ \/ and any other escaped character stand for themselves
    }
  }

  private void consumeNonExecutePrefix() throws IOException {
    int c = nextNonWhitespace(false);
    if (c == -1) return;
    unread(c);
    StringBuilder seen = new StringBuilder();
    for (int i = 0; i < NON_EXECUTE_PREFIX.length(); i++) {
      int next = read();
      if (next == -1) break;
      seen.append((char) next);
      if (next != NON_EXECUTE_PREFIX.charAt(i)) break;
    }
    if (seen.toString().equals(NON_EXECUTE_PREFIX)) return;
    for (int i = seen.length() - 1; i >= 0; i--) unread(seen.charAt(i));
  }

  // The next character outside whitespace and (lenient) comments; -1 or EOFException at the end.
  private int nextNonWhitespace(boolean throwOnEof) throws IOException {
    while (true) {
      int c = read();
      switch (c) {
        case -1:
          if (throwOnEof) throw new EOFException("End of input at line " + line + " column " + column);
          return -1;
        case ' ':
        case '\t':
        case '\n':
        case '\r':
          continue;
        case '/':
          int next = read();
          if (next == '*') {
            checkLenient();
            skipBlockComment();
            continue;
          }
          if (next == '/') {
            checkLenient();
            skipToEndOfLine();
            continue;
          }
          if (next != -1) unread(next);
          return c;
        case '#':
          checkLenient();
          skipToEndOfLine();
          continue;
        default:
          return c;
      }
    }
  }

  private void skipBlockComment() throws IOException {
    int previous = 0;
    while (true) {
      int c = read();
      if (c == -1) throw syntaxError("Unterminated comment");
      if (previous == '*' && c == '/') return;
      previous = c;
    }
  }

  private void skipToEndOfLine() throws IOException {
    while (true) {
      int c = read();
      if (c == -1 || c == '\n' || c == '\r') return;
    }
  }

  private int read() throws IOException {
    int c;
    if (pushback.length() > 0) {
      c = pushback.charAt(pushback.length() - 1);
      pushback.setLength(pushback.length() - 1);
    } else {
      if (pos == limit) {
        limit = in.read(buffer, 0, buffer.length);
        pos = 0;
        if (limit <= 0) {
          limit = 0;
          return -1;
        }
      }
      c = buffer[pos++];
    }
    if (c == '\n') {
      line++;
      column = 1;
    } else {
      column++;
    }
    return c;
  }

  private void unread(int c) {
    if (c == -1) return;
    pushback.append((char) c);
    if (c == '\n') line--;
    else column--;
  }

  private void checkLenient() throws IOException {
    if (!lenient) throw syntaxError("Use JsonReader.setLenient(true) to accept malformed JSON");
  }

  private IOException syntaxError(String message) {
    return new MalformedJsonException(message + " at line " + line + " column " + column);
  }

  @Override
  public String toString() {
    return getClass().getSimpleName() + " at line " + line + " column " + column;
  }
}
