package android.content;
public class ClipData {
  private final Item item;
  public ClipData(CharSequence label, String[] mimeTypes, Item i) {
    item = i;
  }
  public static ClipData newPlainText(CharSequence label, CharSequence text) {
    return new ClipData(label, new String[] {"text/plain"}, new Item(text));
  }
  public int getItemCount() {
    return 1;
  }
  public Item getItemAt(int i) {
    if (i != 0)
      throw new IndexOutOfBoundsException();
    return item;
  }
  public static class Item {
    private final CharSequence text;
    public Item(CharSequence t) {
      text = t;
    }
    public CharSequence getText() {
      return text;
    }
    public CharSequence coerceToText(Context c) {
      return text;
    }
  }
}
