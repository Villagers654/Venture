package com.google.android.play.core.assetpacks;
// Public result ABI: com.google.android.play:core:1.10.3, classes.jar.
// Unity retains these callback types even when AssetPackManagerFactory is absent.
public abstract class AssetPackStates {
  public abstract long totalBytes();
  public abstract java.util.Map<String, AssetPackState> packStates();
}
