package com.google.android.play.core.tasks;
// Public abstract ABI: com.google.android.play:core:1.10.3, classes.jar.
// There is no task producer or Google Play service implementation.
public abstract class Task<T> {
  public abstract boolean isComplete();
  public abstract boolean isSuccessful();
  public abstract T getResult();
  public abstract <X extends Throwable> T getResult(Class<X> exceptionType) throws X;
  public abstract Exception getException();
  public abstract Task<T> addOnCompleteListener(OnCompleteListener<T> listener);
  public abstract Task<T> addOnCompleteListener(
      java.util.concurrent.Executor executor, OnCompleteListener<T> listener);
  public abstract Task<T> addOnFailureListener(OnFailureListener listener);
  public abstract Task<T> addOnFailureListener(
      java.util.concurrent.Executor executor, OnFailureListener listener);
  public abstract Task<T> addOnSuccessListener(OnSuccessListener<? super T> listener);
  public abstract Task<T> addOnSuccessListener(
      java.util.concurrent.Executor executor, OnSuccessListener<? super T> listener);
}
