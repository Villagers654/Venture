package android.webkit;

import java.io.InputStream;
import java.util.Map;

/** AOSP android.webkit.WebResourceResponse: a response supplied by shouldInterceptRequest. */
public class WebResourceResponse {
  private String mimeType, encoding, reasonPhrase;
  private int statusCode = 200;
  private Map<String, String> responseHeaders;
  private InputStream data;
  public WebResourceResponse(String mimeType, String encoding, InputStream data) {
    this.mimeType = mimeType; this.encoding = encoding; this.data = data; reasonPhrase = "OK";
  }
  public WebResourceResponse(String mimeType, String encoding, int statusCode, String reasonPhrase,
      Map<String, String> responseHeaders, InputStream data) {
    this(mimeType, encoding, data);
    setStatusCodeAndReasonPhrase(statusCode, reasonPhrase);
    this.responseHeaders = responseHeaders;
  }
  public void setMimeType(String mimeType) { this.mimeType = mimeType; }
  public String getMimeType() { return mimeType; }
  public void setEncoding(String encoding) { this.encoding = encoding; }
  public String getEncoding() { return encoding; }
  public void setStatusCodeAndReasonPhrase(int statusCode, String reasonPhrase) {
    if (statusCode < 100 || statusCode > 599)
      throw new IllegalArgumentException("statusCode can't be less than 100 or greater than 599.");
    if (statusCode > 299 && statusCode < 400)
      throw new IllegalArgumentException("statusCode can't be in the [300, 399] range.");
    if (reasonPhrase == null) throw new IllegalArgumentException("reasonPhrase can't be null.");
    if (reasonPhrase.trim().isEmpty()) throw new IllegalArgumentException("reasonPhrase can't be empty.");
    for (int i = 0; i < reasonPhrase.length(); i++)
      if (reasonPhrase.charAt(i) > 0x7F)
        throw new IllegalArgumentException("reasonPhrase can't contain non-ASCII characters.");
    this.statusCode = statusCode;
    this.reasonPhrase = reasonPhrase;
  }
  public int getStatusCode() { return statusCode; }
  public String getReasonPhrase() { return reasonPhrase; }
  public void setResponseHeaders(Map<String, String> headers) { responseHeaders = headers; }
  public Map<String, String> getResponseHeaders() { return responseHeaders; }
  public void setData(InputStream data) { this.data = data; }
  public InputStream getData() { return data; }
}
