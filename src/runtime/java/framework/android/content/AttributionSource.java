package android.content;

import android.os.Binder;
import android.os.IBinder;
import android.os.Parcel;
import android.os.Parcelable;
import java.util.Arrays;
import java.util.Collections;
import java.util.Objects;
import java.util.Set;

/**
 * AOSP android.content.AttributionSource (API 31): who an access is attributed to. A context's
 * source carries the app uid and op package name, the context's attribution tag and the default
 * token. Nothing registers sources with a permission service, so none is trusted.
 */
public final class AttributionSource implements Parcelable {
  private static final IBinder DEFAULT_TOKEN = new Binder();
  private final int uid;
  private final String packageName, attributionTag;
  private final IBinder token;
  private final String[] renouncedPermissions;
  private final AttributionSource next;

  /** @hide TestApi */
  public AttributionSource(int uid, String packageName, String attributionTag) {
    this(uid, packageName, attributionTag, DEFAULT_TOKEN);
  }
  /** @hide */
  public AttributionSource(int uid, String packageName, String attributionTag, IBinder token) {
    this(uid, packageName, attributionTag, token, null, null);
  }
  /** @hide */
  public AttributionSource(int uid, String packageName, String attributionTag,
      Set<String> renouncedPermissions, AttributionSource next) {
    this(uid, packageName, attributionTag, DEFAULT_TOKEN,
        renouncedPermissions == null ? null : renouncedPermissions.toArray(new String[0]), next);
  }
  /** @hide TestApi */
  public AttributionSource(AttributionSource current, AttributionSource next) {
    this(current.uid, current.packageName, current.attributionTag, current.token,
        current.renouncedPermissions, next);
  }
  private AttributionSource(int uid, String packageName, String attributionTag, IBinder token,
      String[] renouncedPermissions, AttributionSource next) {
    this.uid = uid;
    this.packageName = packageName;
    this.attributionTag = attributionTag;
    this.token = token;
    // No renounced permissions is one state, whether it came as null or as an empty set.
    this.renouncedPermissions = renouncedPermissions == null || renouncedPermissions.length == 0 ? null
        : renouncedPermissions.clone();
    this.next = next;
  }
  private AttributionSource(Parcel in) {
    this(in.readInt(), in.readString(), in.readString(), in.readStrongBinder(), in.createStringArray(),
        in.readParcelable(AttributionSource.class.getClassLoader()));
  }

  /** The calling app's source: its Application context's, as ActivityThread.currentAttributionSource. */
  public static AttributionSource myAttributionSource() {
    var application = com.venture.runtime.RuntimeEnvironment.get().application;
    if (application != null) return application.getAttributionSource();
    return new Builder(android.os.Process.myUid())
        .setPackageName(com.venture.runtime.RuntimeEnvironment.get().manifest.info.packageName).build();
  }

  public int getUid() { return uid; }
  public String getPackageName() { return packageName; }
  public String getAttributionTag() { return attributionTag; }
  public AttributionSource getNext() { return next; }
  /** @hide TestApi */
  public IBinder getToken() { return token; }
  /** SystemApi. */
  public Set<String> getRenouncedPermissions() {
    return renouncedPermissions == null ? Collections.emptySet()
        : Collections.unmodifiableSet(new java.util.LinkedHashSet<>(Arrays.asList(renouncedPermissions)));
  }
  /** Trust needs the source registered with the permission manager, which no source is here. */
  public boolean isTrusted(Context context) {
    return false;
  }
  public boolean checkCallingUid() {
    int callingUid = Binder.getCallingUid();
    return callingUid == 0 || callingUid % 100000 == 1000 || callingUid == uid;
  }
  public void enforceCallingUid() {
    if (!checkCallingUid())
      throw new SecurityException("Calling uid: " + Binder.getCallingUid() + " doesn't match source uid: " + uid);
  }

  @Override public boolean equals(Object o) {
    if (this == o) return true;
    if (!(o instanceof AttributionSource that)) return false;
    return uid == that.uid && Objects.equals(packageName, that.packageName)
        && Objects.equals(attributionTag, that.attributionTag) && Objects.equals(token, that.token)
        && Arrays.equals(renouncedPermissions, that.renouncedPermissions) && Objects.equals(next, that.next);
  }
  @Override public int hashCode() {
    int result = Integer.hashCode(uid);
    result = 31 * result + Objects.hashCode(packageName);
    result = 31 * result + Objects.hashCode(attributionTag);
    result = 31 * result + Objects.hashCode(token);
    result = 31 * result + Arrays.hashCode(renouncedPermissions);
    result = 31 * result + Objects.hashCode(next);
    return result;
  }
  // User builds (Quest) print Object's form; AOSP prints the fields only on debuggable builds.

  @Override public int describeContents() { return 0; }
  @Override public void writeToParcel(Parcel dest, int flags) {
    dest.writeInt(uid);
    dest.writeString(packageName);
    dest.writeString(attributionTag);
    dest.writeStrongBinder(token);
    dest.writeStringArray(renouncedPermissions);
    dest.writeParcelable(next, flags);
  }
  public static final Parcelable.Creator<AttributionSource> CREATOR = new Parcelable.Creator<>() {
    @Override public AttributionSource createFromParcel(Parcel in) { return new AttributionSource(in); }
    @Override public AttributionSource[] newArray(int size) { return new AttributionSource[size]; }
  };

  public static final class Builder {
    private int uid;
    private String packageName, attributionTag;
    private IBinder token = DEFAULT_TOKEN;
    private String[] renouncedPermissions;
    private AttributionSource next;
    private boolean built;
    public Builder(int uid) {
      this.uid = uid;
    }
    public Builder setUid(int value) {
      checkNotUsed();
      uid = value;
      return this;
    }
    public Builder setPackageName(String value) {
      checkNotUsed();
      packageName = value;
      return this;
    }
    public Builder setAttributionTag(String value) {
      checkNotUsed();
      attributionTag = value;
      return this;
    }
    /** SystemApi. */
    public Builder setRenouncedPermissions(Set<String> value) {
      checkNotUsed();
      renouncedPermissions = value == null ? null : value.toArray(new String[0]);
      return this;
    }
    public Builder setNext(AttributionSource value) {
      checkNotUsed();
      next = value;
      return this;
    }
    /** @hide */
    public Builder setToken(IBinder value) {
      checkNotUsed();
      token = value;
      return this;
    }
    public AttributionSource build() {
      checkNotUsed();
      built = true;
      return new AttributionSource(uid, packageName, attributionTag, token, renouncedPermissions, next);
    }
    private void checkNotUsed() {
      if (built)
        throw new IllegalStateException("This Builder should not be reused. Use a new Builder instance instead");
    }
  }
}
