package android.print;

/**
 * AOSP android.print.PrintDocumentAdapter, lifecycle part only. Venture has no
 * print spooler (a Quest has no print service), so no layout or write pass is
 * ever requested; WebView.createPrintDocumentAdapter returns one of these.
 */
public abstract class PrintDocumentAdapter {
  public static final String EXTRA_PRINT_PREVIEW = "EXTRA_PRINT_PREVIEW";
  public PrintDocumentAdapter() {}
  public void onStart() {}
  public void onFinish() {}
}
