package com.google.android.play.core.assetpacks;
// Public callback ABI: play-services-tasks 18.1.0 / asset-delivery 2.3.0.
public abstract class AssetPackState {
  public abstract String name();
  public abstract int status();
  public abstract int errorCode();
  public abstract long bytesDownloaded();
  public abstract long totalBytesToDownload();
  public abstract int transferProgressPercentage();
}
