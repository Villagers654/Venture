package android.text.style;
public abstract class CharacterStyle {
  public abstract void updateDrawState(android.text.TextPaint paint);
  public CharacterStyle getUnderlying() {
    return this;
  }
}
