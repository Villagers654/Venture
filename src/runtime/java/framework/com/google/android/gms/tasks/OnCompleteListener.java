package com.google.android.gms.tasks;
// Public callback ABI: play-services-tasks 18.1.0 / asset-delivery 2.3.0.
public interface OnCompleteListener<T> {
  void onComplete(Task<T> task);
}
