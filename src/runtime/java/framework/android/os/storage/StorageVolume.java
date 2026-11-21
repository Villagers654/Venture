package android.os.storage;

import android.content.Context;
import android.content.Intent;
import android.os.Parcel;
import android.os.Parcelable;
import java.io.File;
import java.util.Objects;

/**
 * AOSP android.os.storage.StorageVolume: the device's shared storage (primary,
 * emulated) and each folder the person picked through the document picker, a
 * removable volume as an SD card is (com.venture.runtime.HostDocuments). A
 * directory is a host path, as Java File paths are in Venture; native code sees
 * the volume at /storage/<uuid>.
 */
public final class StorageVolume implements Parcelable {
  public static final String EXTRA_STORAGE_VOLUME = "android.os.storage.extra.STORAGE_VOLUME";
  private final String id, uuid, description;
  private final File directory;
  private final boolean primary;

  public StorageVolume(String id, String uuid, File directory, String description, boolean primary) {
    this.id = id;
    this.uuid = uuid;
    this.directory = directory;
    this.description = description;
    this.primary = primary;
  }

  public String getId() { return id; }
  public String getUuid() { return uuid; }
  public File getDirectory() { return directory; }
  public String getPath() { return directory.getPath(); }
  public File getPathFile() { return directory; }
  public String getDescription(Context context) { return description; }
  public boolean isPrimary() { return primary; }
  public boolean isRemovable() { return !primary; }
  public boolean isEmulated() { return primary; }
  public String getState() { return Environment_MEDIA_MOUNTED; }
  public String getMediaStoreVolumeName() { return primary ? "external_primary" : uuid.toLowerCase(java.util.Locale.ROOT); }
  public java.util.UUID getStorageUuid() { return primary ? StorageManager.UUID_DEFAULT : null; }
  private static final String Environment_MEDIA_MOUNTED = "mounted";

  /** The picker for a folder on this volume (Android shows its document tree picker). */
  public Intent createOpenDocumentTreeIntent() {
    return new Intent("android.intent.action.OPEN_DOCUMENT_TREE");
  }

  @Override public boolean equals(Object other) {
    return other instanceof StorageVolume volume && Objects.equals(volume.id, id) && volume.directory.equals(directory);
  }
  @Override public int hashCode() { return Objects.hash(id, directory); }
  @Override public String toString() { return "StorageVolume: " + description; }
  @Override public int describeContents() { return 0; }
  @Override public void writeToParcel(Parcel dest, int flags) {
    dest.writeString(id);
    dest.writeString(uuid);
    dest.writeString(directory.getPath());
    dest.writeString(description);
    dest.writeInt(primary ? 1 : 0);
  }
  public static final Creator<StorageVolume> CREATOR = new Creator<>() {
    @Override public StorageVolume createFromParcel(Parcel in) {
      return new StorageVolume(in.readString(), in.readString(), new File(in.readString()), in.readString(),
          in.readInt() != 0);
    }
    @Override public StorageVolume[] newArray(int size) { return new StorageVolume[size]; }
  };
}
