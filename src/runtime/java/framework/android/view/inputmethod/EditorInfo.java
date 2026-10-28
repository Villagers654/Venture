package android.view.inputmethod;
public class EditorInfo {
  public int inputType, imeOptions, initialSelStart = -1, initialSelEnd = -1, actionId;
  public CharSequence actionLabel, hintText, label;
  public String privateImeOptions, packageName;
  public android.os.Bundle extras;
}
