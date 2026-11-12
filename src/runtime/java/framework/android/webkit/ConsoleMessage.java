package android.webkit;

/** AOSP android.webkit.ConsoleMessage: a JavaScript console message for WebChromeClient. */
public class ConsoleMessage {
  public enum MessageLevel { TIP, LOG, WARNING, ERROR, DEBUG }
  private final MessageLevel level;
  private final String message, sourceId;
  private final int lineNumber;
  public ConsoleMessage(String message, String sourceId, int lineNumber, MessageLevel msgLevel) {
    this.message = message; this.sourceId = sourceId; this.lineNumber = lineNumber; this.level = msgLevel;
  }
  public MessageLevel messageLevel() { return level; }
  public String message() { return message; }
  public String sourceId() { return sourceId; }
  public int lineNumber() { return lineNumber; }
}
