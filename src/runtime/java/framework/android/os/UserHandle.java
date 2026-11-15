package android.os;

/** AOSP android.os.UserHandle: a Quest runs apps as its single owner user, id 0. */
public final class UserHandle {
  public static final int PER_USER_RANGE = 100000;
  public static final int USER_SYSTEM = 0;
  public static final UserHandle SYSTEM = new UserHandle(USER_SYSTEM);
  private final int handle;
  public UserHandle(int handle) { this.handle = handle; }
  public static UserHandle of(int userId) { return userId == USER_SYSTEM ? SYSTEM : new UserHandle(userId); }
  public static UserHandle getUserHandleForUid(int uid) { return of(getUserId(uid)); }
  public static int getUserId(int uid) { return uid / PER_USER_RANGE; }
  public static int getAppId(int uid) { return uid % PER_USER_RANGE; }
  public static int myUserId() { return getUserId(Process.myUid()); }
  public int getIdentifier() { return handle; }
  public boolean isSystem() { return handle == USER_SYSTEM; }
  @Override public boolean equals(Object o) { return o instanceof UserHandle other && other.handle == handle; }
  @Override public int hashCode() { return handle; }
  @Override public String toString() { return "UserHandle{" + handle + "}"; }
}
