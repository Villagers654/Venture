package com.google.android.gms.tasks;
// Public callback ABI: play-services-tasks 18.1.0 / asset-delivery 2.3.0.
/**
 * Abstract result contract only. Venture provides no Google task producer or
 * service.
 */
public abstract class Task<T> {
  public abstract boolean isComplete();
  public abstract boolean isSuccessful();
  public abstract boolean isCanceled();
  public abstract T getResult();
  public abstract <X extends Throwable> T getResult(Class<X> exceptionType) throws X;
  public abstract Exception getException();
  public abstract Task<T> addOnFailureListener(OnFailureListener listener);
  public abstract Task<T> addOnFailureListener(
      android.app.Activity activity, OnFailureListener listener);
  public abstract Task<T> addOnFailureListener(
      java.util.concurrent.Executor executor, OnFailureListener listener);
  public abstract Task<T> addOnSuccessListener(OnSuccessListener<? super T> listener);
  public abstract Task<T> addOnSuccessListener(
      android.app.Activity activity, OnSuccessListener<? super T> listener);
  public abstract Task<T> addOnSuccessListener(
      java.util.concurrent.Executor executor, OnSuccessListener<? super T> listener);
}
