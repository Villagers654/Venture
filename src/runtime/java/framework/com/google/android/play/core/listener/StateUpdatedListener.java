package com.google.android.play.core.listener;
// Public callback ABI: play-services-tasks 18.1.0 / asset-delivery 2.3.0.
public interface StateUpdatedListener<T> {
  void onStateUpdate(T state);
}
