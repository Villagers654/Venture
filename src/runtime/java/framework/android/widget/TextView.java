package android.widget;
public class TextView extends android.view.View {
  private android.text.Editable text = new android.text.SpannableStringBuilder();
  private android.text.Editable.Factory editableFactory = android.text.Editable.Factory.getInstance();
  private CharSequence hint = "";
  private int hintColor;
  public void setHintTextColor(int color) { hintColor = color; invalidate(); }
  public int getCurrentHintTextColor() { return hintColor; }
  public int getSelectionStart() { return android.text.Selection.getSelectionStart(text); }
  public int getSelectionEnd() { return android.text.Selection.getSelectionEnd(text); }
  protected void onSelectionChanged(int start, int end) {}
  public final void append(CharSequence value) { append(value, 0, value.length()); }
  public void append(CharSequence value, int start, int end) { text.append(value, start, end); }
  private final java.util.List<android.text.TextWatcher> watchers = new java.util.concurrent.CopyOnWriteArrayList<>();
  private android.text.InputFilter[] filters = {};
  public enum BufferType { NORMAL, SPANNABLE, EDITABLE }
  private int color, gravity, inputType;
  private int imeOptions, maxLines = Integer.MAX_VALUE;
  private boolean singleLine;
  private android.text.method.TransformationMethod transformation;
  private android.text.method.KeyListener keyListener;
  private OnEditorActionListener editorListener;
  public TextView(android.content.Context c) {
    super(c);
  }
  public TextView(android.content.Context c, android.util.AttributeSet a) {
    super(c, a);
  }
  public TextView(android.content.Context c, android.util.AttributeSet a, int style) {
    super(c, a, style);
    if (a != null || style != 0)
      android.util.Log.w("TextView", "Text editing is available; themed Java view rendering is unavailable");
  }
  public void setText(CharSequence value) {
    var filtered = new android.text.SpannableStringBuilder();
    filtered.setFilters(filters);
    filtered.append(value == null ? "" : value);
    var replacement = java.util.Objects.requireNonNull(editableFactory.newEditable(filtered));
    replacement.setFilters(filters);
    int before = text.length(), after = replacement.length();
    for (var watcher : watchers) watcher.beforeTextChanged(text, 0, before, after);
    text = replacement;
    for (var watcher : watchers) text.setSpan(watcher, 0, after, 18);
    for (var watcher : watchers) watcher.onTextChanged(text, 0, before, after);
    for (var watcher : watchers) watcher.afterTextChanged(text);
  }
  public final void setEditableFactory(android.text.Editable.Factory factory) {
    editableFactory = java.util.Objects.requireNonNull(factory);
    setText(text);
  }
  public void setText(CharSequence value, BufferType type) { setText(value); }
  public void addTextChangedListener(android.text.TextWatcher watcher) {
    watchers.add(java.util.Objects.requireNonNull(watcher));
    text.setSpan(watcher, 0, text.length(), 18); // SPAN_INCLUSIVE_INCLUSIVE
  }
  public void removeTextChangedListener(android.text.TextWatcher watcher) {
    watchers.remove(watcher); text.removeSpan(watcher);
  }
  public void setFilters(android.text.InputFilter[] filters) {
    this.filters = filters.clone(); text.setFilters(filters);
  }
  public android.text.InputFilter[] getFilters() { return filters.clone(); }
  public void setText(int id) {
    setText(getResources().getText(id));
  }
  public CharSequence getText() {
    return text;
  }
  public void setHint(CharSequence value) {
    hint = value;
  }
  public CharSequence getHint() {
    return hint;
  }
  public void setTextColor(int c) {
    color = c;
  }
  public int getCurrentTextColor() { return color; }
  private int highlightColor = 0x6633b5e5;
  private boolean cursorVisible = true, textSelectable;
  public void setHighlightColor(int color) { highlightColor = color; }
  public int getHighlightColor() { return highlightColor; }
  public void setCursorVisible(boolean visible) { cursorVisible = visible; }
  public boolean isCursorVisible() { return cursorVisible; }
  public void setTextIsSelectable(boolean selectable) { textSelectable = selectable; }
  public boolean isTextSelectable() { return textSelectable; }
  public void setGravity(int g) {
    gravity = g;
  }
  public void setInputType(int t) {
    inputType = t;
  }
  public void setRawInputType(int t) { inputType = t; }
  public void setImeOptions(int options) { imeOptions = options; }
  public int getImeOptions() { return imeOptions; }
  public void setSingleLine(boolean singleLine) {
    this.singleLine = singleLine;
    maxLines = singleLine ? 1 : Integer.MAX_VALUE;
    inputType = singleLine ? inputType & ~0x20000 : inputType | 0x20000;
  }
  public void setSingleLine() { setSingleLine(true); }
  public boolean isSingleLine() { return singleLine; }
  public void setMaxLines(int lines) {
    if (lines < 0) throw new IllegalArgumentException("Negative line count");
    maxLines = lines;
  }
  public int getMaxLines() { return maxLines; }
  public void setTransformationMethod(android.text.method.TransformationMethod method) { transformation = method; }
  public android.text.method.TransformationMethod getTransformationMethod() { return transformation; }
  public boolean onCheckIsTextEditor() { return inputType != 0; }
  public android.view.inputmethod.InputConnection onCreateInputConnection(android.view.inputmethod.EditorInfo info) {
    if (!onCheckIsTextEditor()) return null;
    info.inputType = inputType; info.imeOptions = imeOptions; info.hintText = hint;
    info.initialSelStart = android.text.Selection.getSelectionStart(text);
    info.initialSelEnd = android.text.Selection.getSelectionEnd(text);
    return new android.view.inputmethod.BaseInputConnection(this, true);
  }
  public void onEditorAction(int action) {
    if (editorListener != null) editorListener.onEditorAction(this, action, null);
  }
  public int getInputType() {
    return inputType;
  }
  public android.text.method.KeyListener getKeyListener() { return keyListener; }
  public void setKeyListener(android.text.method.KeyListener listener) {
    keyListener = listener;
    inputType = listener == null ? 0 : listener.getInputType();
    if (listener != null) setFocusable(true);
  }
  @Override public boolean onKeyDown(int code, android.view.KeyEvent event) {
    return (keyListener != null && keyListener.onKeyDown(this, text, code, event)) || super.onKeyDown(code, event);
  }
  @Override public boolean onKeyUp(int code, android.view.KeyEvent event) {
    return (keyListener != null && keyListener.onKeyUp(this, text, code, event)) || super.onKeyUp(code, event);
  }
  public void setOnEditorActionListener(OnEditorActionListener l) {
    editorListener = l;
  }
  // Size, font and layout attributes: kept for the game to read back (Venture
  // draws no Java views).
  private float textSize = 14f * 2f;  // 14sp at the reported density
  private android.graphics.Typeface typeface = android.graphics.Typeface.DEFAULT;
  private float lineSpacingExtra, lineSpacingMultiplier = 1f;
  private boolean allCaps, includeFontPadding = true, horizontallyScrolling;
  private int minLines, linkColor, compoundPadding, maxWidth = Integer.MAX_VALUE, minWidth;
  private android.text.TextUtils.TruncateAt ellipsize;
  private android.text.method.MovementMethod movement;
  public void setTextSize(float size) { setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, size); }
  public void setTextSize(int unit, float size) {
    android.util.DisplayMetrics metrics;
    try {
      metrics = getResources().getDisplayMetrics();
    } catch (IllegalStateException e) {  // a view built before the app's resources exist
      metrics = new android.util.DisplayMetrics();
    }
    textSize = android.util.TypedValue.applyDimension(unit, size, metrics);
    invalidate();
  }
  public float getTextSize() { return textSize; }
  public void setTypeface(android.graphics.Typeface tf) { typeface = tf; invalidate(); }
  public void setTypeface(android.graphics.Typeface tf, int style) {
    typeface = style > 0 ? android.graphics.Typeface.create(tf, style) : tf;
    invalidate();
  }
  public android.graphics.Typeface getTypeface() { return typeface; }
  public void setLineSpacing(float add, float mult) { lineSpacingExtra = add; lineSpacingMultiplier = mult; }
  public float getLineSpacingExtra() { return lineSpacingExtra; }
  public float getLineSpacingMultiplier() { return lineSpacingMultiplier; }
  public void setAllCaps(boolean allCaps) { this.allCaps = allCaps; }
  public boolean isAllCaps() { return allCaps; }
  public void setIncludeFontPadding(boolean include) { includeFontPadding = include; }
  public boolean getIncludeFontPadding() { return includeFontPadding; }
  public void setHorizontallyScrolling(boolean whether) { horizontallyScrolling = whether; }
  public void setLines(int lines) { minLines = lines; setMaxLines(lines); }
  public void setMinLines(int lines) { minLines = lines; }
  public int getMinLines() { return minLines; }
  public void setEms(int ems) {}
  public void setMaxEms(int ems) {}
  public void setMinEms(int ems) {}
  public void setMaxWidth(int pixels) { maxWidth = pixels; }
  public int getMaxWidth() { return maxWidth; }
  public void setMinWidth(int pixels) { minWidth = pixels; }
  public int getMinWidth() { return minWidth; }
  public void setEllipsize(android.text.TextUtils.TruncateAt where) { ellipsize = where; }
  public android.text.TextUtils.TruncateAt getEllipsize() { return ellipsize; }
  public void setShadowLayer(float radius, float dx, float dy, int color) {}
  public void setLinkTextColor(int color) { linkColor = color; }
  public void setTextColor(android.content.res.ColorStateList colors) {
    if (colors != null) setTextColor(colors.getDefaultColor());
  }
  public void setMovementMethod(android.text.method.MovementMethod method) { movement = method; }
  public android.text.method.MovementMethod getMovementMethod() { return movement; }
  public void setCompoundDrawablePadding(int pad) { compoundPadding = pad; }
  public int getCompoundDrawablePadding() { return compoundPadding; }
  public void setCompoundDrawables(android.graphics.drawable.Drawable left, android.graphics.drawable.Drawable top,
                                   android.graphics.drawable.Drawable right, android.graphics.drawable.Drawable bottom) {}
  public void setCompoundDrawablesWithIntrinsicBounds(int left, int top, int right, int bottom) {}
  public void setCompoundDrawablesWithIntrinsicBounds(android.graphics.drawable.Drawable left,
      android.graphics.drawable.Drawable top, android.graphics.drawable.Drawable right,
      android.graphics.drawable.Drawable bottom) {}
  public int getLineCount() { return text.length() == 0 ? 0 : text.toString().split("\n", -1).length; }
  public int length() { return text.length(); }
  public interface OnEditorActionListener {
    boolean onEditorAction(TextView view, int actionId, android.view.KeyEvent event);
  }
}
