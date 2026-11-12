package android.net.http;

/** AOSP android.net.http.SslError: the TLS errors of a page load, as a bit set. */
public class SslError {
  public static final int SSL_NOTYETVALID = 0, SSL_EXPIRED = 1, SSL_IDMISMATCH = 2,
      SSL_UNTRUSTED = 3, SSL_DATE_INVALID = 4, SSL_INVALID = 5, SSL_MAX_ERROR = 6;
  private int errors;
  private final String url;
  public SslError(int error, String url) {
    this.url = url;
    addError(error);
  }
  public String getUrl() { return url; }
  public boolean addError(int error) {
    boolean valid = error >= 0 && error < SSL_MAX_ERROR;
    if (valid) errors |= 1 << error;
    return valid;
  }
  public boolean hasError(int error) {
    return error >= 0 && error < SSL_MAX_ERROR && (errors & (1 << error)) != 0;
  }
  public int getPrimaryError() {
    for (int error = SSL_MAX_ERROR - 1; error >= 0; --error)
      if ((errors & (1 << error)) != 0) return error;
    return -1;
  }
  @Override public String toString() { return "primary error: " + getPrimaryError() + " url: " + url; }
}
