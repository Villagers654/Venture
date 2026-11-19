package android.view.inputmethod;
public class EditorInfo implements android.text.InputType {
  public static final int IME_MASK_ACTION = 0xff, IME_ACTION_UNSPECIFIED = 0, IME_ACTION_NONE = 1,
      IME_ACTION_GO = 2, IME_ACTION_SEARCH = 3, IME_ACTION_SEND = 4, IME_ACTION_NEXT = 5, IME_ACTION_DONE = 6,
      IME_ACTION_PREVIOUS = 7, IME_FLAG_NO_PERSONALIZED_LEARNING = 0x1000000, IME_FLAG_NO_FULLSCREEN = 0x2000000,
      IME_FLAG_NAVIGATE_PREVIOUS = 0x4000000, IME_FLAG_NAVIGATE_NEXT = 0x8000000, IME_FLAG_NO_EXTRACT_UI = 0x10000000,
      IME_FLAG_NO_ACCESSORY_ACTION = 0x20000000, IME_FLAG_NO_ENTER_ACTION = 0x40000000,
      IME_FLAG_FORCE_ASCII = 0x80000000, IME_NULL = 0;
  public int inputType, imeOptions, initialSelStart = -1, initialSelEnd = -1, actionId;
  public CharSequence actionLabel, hintText, label;
  public String privateImeOptions, packageName;
  public android.os.Bundle extras;
  public int initialCapsMode, fieldId;
  public String fieldName;
}
