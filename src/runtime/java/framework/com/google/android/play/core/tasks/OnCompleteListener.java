package com.google.android.play.core.tasks;
// Public callback ABI: com.google.android.play:core:1.10.3, classes.jar.
public interface OnCompleteListener<T> {
  void onComplete(Task<T> task);
}
