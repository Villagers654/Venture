package android.view;
public class MenuInflater {
  protected final android.content.Context context;
  public MenuInflater(android.content.Context c) {
    context = c;
  }
  public void inflate(int resource, Menu menu) {
    throw new UnsupportedOperationException("Menu resource inflation");
  }
}
