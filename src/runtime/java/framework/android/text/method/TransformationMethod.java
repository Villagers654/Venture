package android.text.method;
public interface TransformationMethod {
  CharSequence getTransformation(CharSequence source, android.view.View view);
  void onFocusChanged(android.view.View view, CharSequence sourceText, boolean focused,
      int direction, android.graphics.Rect previouslyFocusedRect);
}
